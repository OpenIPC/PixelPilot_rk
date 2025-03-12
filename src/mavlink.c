#define _GNU_SOURCE
#include <sys/prctl.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <sys/prctl.h>
#include <sys/sem.h>

#include "mavlink/common/mavlink.h"
#include "mavlink.h"
#include "osd.h"

// Declare the C-compatible interface to cpp dvr functions
typedef struct Dvr* Dvr; // Forward declaration
void dvr_start_recording(Dvr* dvr);
void dvr_stop_recording(Dvr* dvr);
extern Dvr *dvr;

#define earthRadiusKm 6371.0
#define BILLION 1000000000L

# define M_PI   3.14159265358979323846  /* pi */

double deg2rad(double degrees) {
    return degrees * M_PI / 180.0;
}

double distanceEarth(double lat1d, double lon1d, double lat2d, double lon2d) {
  double lat1r, lon1r, lat2r, lon2r, u, v;
  lat1r = deg2rad(lat1d);
  lon1r = deg2rad(lon1d);
  lat2r = deg2rad(lat2d);
  lon2r = deg2rad(lon2d);
  u = sin((lat2r - lat1r) / 2);
  v = sin((lon2r - lon1r) / 2);

  return 2.0 * earthRadiusKm * asin(sqrt(u * u + cos(lat1r) * cos(lat2r) * v * v));
}

size_t numOfChars(const char s[]) {
  size_t n = 0;
  while (s[n] != '\0') {
    ++n;
  }

  return n;
}

char* insertString(char s1[], const char s2[], size_t pos) {
  size_t n1 = numOfChars(s1);
  size_t n2 = numOfChars(s2);
  if (n1 < pos) {
    pos = n1;
  }

  for (size_t i = 0; i < n1 - pos; i++) {
    s1[n1 + n2 - i - 1] = s1[n1 - i - 1];
  }

  for (size_t i = 0; i < n2; i++) {
    s1[pos + i] = s2[i];
  }

  s1[n1 + n2] = '\0';

  return s1;
}

int mavlink_port = 14550;
int mavlink_thread_signal = 0;

void* __MAVLINK_THREAD__(void* arg) {
  pthread_setname_np(pthread_self(), "__MAVLINK");
  printf("Starting mavlink thread...\n");
  // Create socket
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    printf("ERROR: Unable to create MavLink socket: %s\n", strerror(errno));
    return 0;
  }

  // Bind port
  struct sockaddr_in addr = {};
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  inet_pton(AF_INET, "0.0.0.0", &(addr.sin_addr));
  addr.sin_port = htons(mavlink_port);

  if (bind(fd, (struct sockaddr*)(&addr), sizeof(addr)) != 0) {
    printf("ERROR: Unable to bind MavLink port: %s\n", strerror(errno));
    return 0;
  }

  // Set Rx timeout
  struct timeval tv;
  tv.tv_sec = 0;
  tv.tv_usec = 100000;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
    printf("ERROR: Unable to bind MavLink rx timeout: %s\n", strerror(errno));
    return 0;
  }

  char buffer[2048];
  while (!mavlink_thread_signal) {
    memset(buffer, 0x00, sizeof(buffer));
    int ret = recv(fd, buffer, sizeof(buffer), 0);
    if (ret < 0) {
      continue;
    } else if (ret == 0) {
      // peer has done an orderly shutdown
      return 0;
    }

    // Parse
    // Credit to openIPC:https://github.com/OpenIPC/silicon_research/blob/master/vdec/main.c#L1020
    mavlink_message_t message;
    mavlink_status_t status;
    static int current_arm_state = -1;
    for (int i = 0; i < ret; ++i) {
      if (mavlink_parse_char(MAVLINK_COMM_0, buffer[i], &message, &status) == 1) {
        osd_tag tags[3];
        strcpy(tags[0].key, "sysid");
        snprintf(tags[0].val, sizeof(tags[0].val), "%d", message.sysid);
        strcpy(tags[1].key, "compid");
        snprintf(tags[1].val, sizeof(tags[1].val), "%d", message.compid);
        switch (message.msgid) {
          case MAVLINK_MSG_ID_HEARTBEAT:
            {
              mavlink_heartbeat_t heartbeat = {};
              mavlink_msg_heartbeat_decode(&message, &heartbeat);
              int received_arm_state = (heartbeat.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0;
              if (current_arm_state != received_arm_state) {
                  osd_publish_bool_fact("mavlink.heartbeet.base_mode.armed", tags, 2, received_arm_state);
                  current_arm_state = received_arm_state;
                  if (mavlink_dvr_on_arm) {
                    if (received_arm_state) {
                      dvr_start_recording(dvr);
                    } else {
                      dvr_stop_recording(dvr);
                    }
                  }
              }
            }
            break;
	      case MAVLINK_MSG_ID_RAW_IMU:
            {
              mavlink_raw_imu_t imu;
              void *batch = osd_batch_init(7);
              mavlink_msg_raw_imu_decode(&message, &imu);
              strcpy(tags[2].key, "imu_id");
              snprintf(tags[2].val, sizeof(tags[2].val), "%d", imu.id);
              osd_add_int_fact(batch, "mavlink.raw_imu.xacc", tags, 3, (long) imu.xacc);
              osd_add_int_fact(batch, "mavlink.raw_imu.yacc", tags, 3, (long) imu.yacc);
              osd_add_int_fact(batch, "mavlink.raw_imu.zacc", tags, 3, (long) imu.zacc);
              osd_add_int_fact(batch, "mavlink.raw_imu.xgyro", tags, 3, (long) imu.xgyro);
              osd_add_int_fact(batch, "mavlink.raw_imu.ygyro", tags, 3, (long) imu.ygyro);
              osd_add_int_fact(batch, "mavlink.raw_imu.zgyro", tags, 3, (long) imu.zgyro);
              osd_add_int_fact(batch, "mavlink.raw_imu.temperature", tags, 3, (long) imu.temperature);
              osd_publish_batch(batch);
            }
            break;
		
          case MAVLINK_MSG_ID_SYS_STATUS:
            {
              mavlink_sys_status_t bat;
              void *batch = osd_batch_init(2);
              mavlink_msg_sys_status_decode(&message, &bat);
              osd_add_uint_fact(batch, "mavlink.sys_status.voltage_battery", tags, 2, (ulong) bat.voltage_battery);
              osd_add_int_fact(batch, "mavlink.sys_status.current_battery", tags, 2, (long) bat.current_battery);
              osd_publish_batch(batch);
            }
            break;

          case MAVLINK_MSG_ID_BATTERY_STATUS:
            {
              mavlink_battery_status_t batt;
              void *batch = osd_batch_init(2);
              mavlink_msg_battery_status_decode(&message, &batt);
              strcpy(tags[2].key, "battery_id");
              snprintf(tags[2].val, sizeof(tags[2].val), "%d", batt.id);
              osd_add_int_fact(batch, "mavlink.battery_status.current_consumed", tags, 3, (long) batt.current_consumed);
              osd_add_int_fact(batch, "mavlink.battery_status.energy_consumed", tags, 3, (long) batt.energy_consumed);
              osd_publish_batch(batch);
            }
            break;

          case MAVLINK_MSG_ID_RC_CHANNELS_RAW:
            {
              mavlink_rc_channels_raw_t rc_channels_raw;
              void *batch = osd_batch_init(8);
              mavlink_msg_rc_channels_raw_decode( &message, &rc_channels_raw);
              osd_add_uint_fact(batch, "mavlink.rc_channels_raw.chan1", tags, 2, (ulong) rc_channels_raw.chan1_raw);
              osd_add_uint_fact(batch, "mavlink.rc_channels_raw.chan2", tags, 2, (ulong) rc_channels_raw.chan2_raw);
              osd_add_uint_fact(batch, "mavlink.rc_channels_raw.chan3", tags, 2, (ulong) rc_channels_raw.chan3_raw);
              osd_add_uint_fact(batch, "mavlink.rc_channels_raw.chan4", tags, 2, (ulong) rc_channels_raw.chan4_raw);
              osd_add_uint_fact(batch, "mavlink.rc_channels_raw.chan5", tags, 2, (ulong) rc_channels_raw.chan5_raw);
              osd_add_uint_fact(batch, "mavlink.rc_channels_raw.chan6", tags, 2, (ulong) rc_channels_raw.chan6_raw);
              osd_add_uint_fact(batch, "mavlink.rc_channels_raw.chan7", tags, 2, (ulong) rc_channels_raw.chan7_raw);
              osd_add_uint_fact(batch, "mavlink.rc_channels_raw.chan8", tags, 2, (ulong) rc_channels_raw.chan8_raw);
              osd_publish_batch(batch);
            }
            break;

          case MAVLINK_MSG_ID_GPS_RAW_INT:
            {
              mavlink_gps_raw_int_t gps;
              void *batch = osd_batch_init(7);
              mavlink_msg_gps_raw_int_decode(&message, &gps);
              osd_add_int_fact(batch, "mavlink.gps_raw.lat", tags, 2, (long) gps.lat); //degE7
              osd_add_int_fact(batch, "mavlink.gps_raw.lon", tags, 2, (long) gps.lon); //degE7
              osd_add_int_fact(batch, "mavlink.gps_raw.alt", tags, 2, (long) gps.alt); //mm
              osd_add_uint_fact(batch, "mavlink.gps_raw.vel", tags, 2, (ulong) gps.vel); //cm/s
              osd_add_uint_fact(batch, "mavlink.gps_raw.cog", tags, 2, (ulong) gps.cog); //cdeg
              osd_add_uint_fact(batch, "mavlink.gps_raw.satellites_visible", tags, 2, (ulong) gps.satellites_visible);
              // Fix type: https://mavlink.io/en/messages/common.html#GPS_FIX_TYPE
              osd_add_uint_fact(batch, "mavlink.gps_raw.fix_type", tags, 2, (ulong) gps.fix_type);
              osd_publish_batch(batch);
            }
            break;

          case MAVLINK_MSG_ID_VFR_HUD:
            {
              mavlink_vfr_hud_t vfr;
              void *batch = osd_batch_init(6);
              mavlink_msg_vfr_hud_decode(&message, &vfr);
              osd_add_double_fact(batch, "mavlink.vfr_hud.airspeed", tags, 2, (double) vfr.airspeed);
              osd_add_double_fact(batch, "mavlink.vfr_hud.groundspeed", tags, 2, (double) vfr.groundspeed);
              osd_add_double_fact(batch, "mavlink.vfr_hud.alt", tags, 2, (double) vfr.alt);
              osd_add_double_fact(batch, "mavlink.vfr_hud.climb", tags, 2, (double) vfr.climb);
              osd_add_int_fact(batch, "mavlink.vfr_hud.heading", tags, 2, (long) vfr.heading);
              osd_add_uint_fact(batch, "mavlink.vfr_hud.throttle", tags, 2, (ulong) vfr.throttle);
              osd_publish_batch(batch);
            }
            break;

          case MAVLINK_MSG_ID_GLOBAL_POSITION_INT:
            {
              mavlink_global_position_int_t global_position_int;
              void *batch = osd_batch_init(8);
              mavlink_msg_global_position_int_decode( &message, &global_position_int);
              osd_add_int_fact(batch, "mavlink.global_position_int.lat", tags, 2, (long) global_position_int.lat); //degE7
              osd_add_int_fact(batch, "mavlink.global_position_int.lon", tags, 2, (long) global_position_int.lon); //degE7
              osd_add_int_fact(batch, "mavlink.global_position_int.alt", tags, 2, (long) global_position_int.alt); //mm
              osd_add_int_fact(batch, "mavlink.global_position_int.relative_alt", tags, 2, (long) global_position_int.relative_alt); //mm
              osd_add_int_fact(batch, "mavlink.global_position_int.vx", tags, 2, (long) global_position_int.vx); //cm/s
              osd_add_int_fact(batch, "mavlink.global_position_int.vy", tags, 2, (long) global_position_int.vy); //cm/s
              osd_add_int_fact(batch, "mavlink.global_position_int.vz", tags, 2, (long) global_position_int.vz); //cm/s
              osd_add_uint_fact(batch, "mavlink.global_position_int.hdg", tags, 2, (ulong) global_position_int.hdg); //cdeg
              osd_publish_batch(batch);
            }
            break;

          case MAVLINK_MSG_ID_ATTITUDE:
            {
              mavlink_attitude_t att;
              void *batch = osd_batch_init(6);
              mavlink_msg_attitude_decode(&message, &att);
              osd_add_double_fact(batch, "mavlink.attitude.roll", tags, 2, (double) att.roll);
              osd_add_double_fact(batch, "mavlink.attitude.pitch", tags, 2, (double) att.pitch);
              osd_add_double_fact(batch, "mavlink.attitude.yaw", tags, 2, (double) att.yaw);
              osd_add_double_fact(batch, "mavlink.attitude.rollspeed", tags, 2, (double) att.rollspeed);
              osd_add_double_fact(batch, "mavlink.attitude.pitchpeed", tags, 2, (double) att.pitchspeed);
              osd_add_double_fact(batch, "mavlink.attitude.yawspeed", tags, 2, (double) att.yawspeed);
              osd_publish_batch(batch);
            }
            break;

          case MAVLINK_MSG_ID_RADIO_STATUS:
            {
                mavlink_radio_status_t radio;
                mavlink_msg_radio_status_decode(&message, &radio);
                void *batch = osd_batch_init(6);
                osd_add_uint_fact(batch, "mavlink.radio_status.rxerrors", tags, 2, (ulong) radio.rxerrors);
                osd_add_uint_fact(batch, "mavlink.radio_status.fixed", tags, 2, (ulong) radio.fixed);
                osd_add_int_fact(batch, "mavlink.radio_status.rssi", tags, 2, (int8_t) radio.rssi); // is type correct?
                osd_add_int_fact(batch, "mavlink.radio_status.remrssi", tags, 2, (long) radio.remrssi); // is type correct?
                osd_add_uint_fact(batch, "mavlink.radio_status.noise", tags, 2, (ulong) radio.noise);
                osd_add_uint_fact(batch, "mavlink.radio_status.remnoise", tags, 2, (ulong) radio.remnoise);
                osd_publish_batch(batch);
                
                if ((message.sysid != 3) || (message.compid != 68)) {
                    break;
                }
            }
            break;

          default:
            // printf("> MavLink message %d from %d/%d\n",
            //   message.msgid, message.sysid, message.compid);
            break;
        }
      }
    }

    usleep(1);
  }
  
	printf("Mavlink thread done.\n");
  return 0;
}
