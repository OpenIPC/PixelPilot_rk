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

// Declare the C-compatible interface functions
#ifdef __cplusplus
extern "C" {
#endif

typedef struct Dvr* Dvr; // Forward declaration

void dvr_start_recording(Dvr* dvr);
void dvr_stop_recording(Dvr* dvr);

#ifdef __cplusplus
}
#endif

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
    static bool current_arm_state = false;
    for (int i = 0; i < ret; ++i) {
      if (mavlink_parse_char(MAVLINK_COMM_0, buffer[i], &message, &status) == 1) {
        switch (message.msgid) {
            case MAVLINK_MSG_ID_HEARTBEAT: {
              if (mavlink_rec_on_arm) {
                mavlink_heartbeat_t heartbeat = {};
                mavlink_msg_heartbeat_decode(&message, &heartbeat);
                bool received_arm_state = (heartbeat.base_mode & MAV_MODE_FLAG_SAFETY_ARMED) != 0;
                if (current_arm_state != received_arm_state) {
                    current_arm_state = received_arm_state;
                    if (received_arm_state) {
                        dvr_start_recording(dvr);
                    } else {
                        dvr_stop_recording(dvr);
                    }
                }
              }
              break;
            }
	  case MAVLINK_MSG_ID_RAW_IMU:
            {
              mavlink_raw_imu_t imu;
              mavlink_msg_raw_imu_decode(&message, &imu);
              osd_vars.telemetry_raw_imu = imu.temperature;
            }
            break;
		
          case MAVLINK_MSG_ID_SYS_STATUS:
            {
              mavlink_sys_status_t bat;
              mavlink_msg_sys_status_decode(&message, &bat);
              osd_vars.telemetry_battery = bat.voltage_battery;
              osd_vars.telemetry_current = bat.current_battery;
            }
            break;

          case MAVLINK_MSG_ID_BATTERY_STATUS:
            {
              mavlink_battery_status_t batt;
              mavlink_msg_battery_status_decode(&message, &batt);
              osd_vars.telemetry_current_consumed = batt.current_consumed;
            }
            break;

          case MAVLINK_MSG_ID_RC_CHANNELS_RAW:
            {
              mavlink_rc_channels_raw_t rc_channels_raw;
              mavlink_msg_rc_channels_raw_decode( &message, &rc_channels_raw);
              osd_vars.telemetry_rssi = rc_channels_raw.rssi;
              osd_vars.telemetry_throttle = (rc_channels_raw.chan4_raw - 1000) / 10;

              if (osd_vars.telemetry_throttle < 0) {
                osd_vars.telemetry_throttle = 0;
              }
              osd_vars.telemetry_arm = rc_channels_raw.chan5_raw;
              osd_vars.telemetry_resolution = rc_channels_raw.chan8_raw;
              if (osd_vars.telemetry_resolution > 1700) {
                system("/root/resolution.sh");
            }
            }
            break;

          case MAVLINK_MSG_ID_GPS_RAW_INT:
            {
              mavlink_gps_raw_int_t gps;
              mavlink_msg_gps_raw_int_decode(&message, &gps);
              osd_vars.telemetry_sats = gps.satellites_visible;
              osd_vars.telemetry_lat = gps.lat;
              osd_vars.telemetry_lon = gps.lon;
              if (osd_vars.telemetry_arm > 1700) {
                if (osd_vars.armed < 1) {
                  osd_vars.armed = 1;
                  osd_vars.telemetry_lat_base = osd_vars.telemetry_lat;
                  osd_vars.telemetry_lon_base = osd_vars.telemetry_lon;
                }

                sprintf(osd_vars.s1, "%.00f", osd_vars.telemetry_lat);
                if (osd_vars.telemetry_lat < 10000000) {
                  insertString(osd_vars.s1, "0.", 0);
                }
                if (osd_vars.telemetry_lat > 9999999) {
                  if (numOfChars(osd_vars.s1) == 8) {
                    insertString(osd_vars.s1, ".", 1);
                  } else {
                    insertString(osd_vars.s1, ".", 2);
                  }
                }

                sprintf(osd_vars.s2, "%.00f", osd_vars.telemetry_lon);
                if (osd_vars.telemetry_lon < 10000000) {
                  insertString(osd_vars.s2, "0.", 0);
                }
                if (osd_vars.telemetry_lon > 9999999) {
                  if (numOfChars(osd_vars.s2) == 8) {
                    insertString(osd_vars.s2, ".", 1);
                  } else {
                    insertString(osd_vars.s2, ".", 2);
                  }
                }

                sprintf(osd_vars.s3, "%.00f", osd_vars.telemetry_lat_base);
                if (osd_vars.telemetry_lat_base < 10000000) {
                  insertString(osd_vars.s3, "0.", 0);
                }
                if (osd_vars.telemetry_lat_base > 9999999) {
                  if (numOfChars(osd_vars.s3) == 8) {
                    insertString(osd_vars.s3, ".", 1);
                  } else {
                    insertString(osd_vars.s3, ".", 2);
                  }
                }

                sprintf(osd_vars.s4, "%.00f", osd_vars.telemetry_lon_base);
                if (osd_vars.telemetry_lon_base < 10000000) {
                  insertString(osd_vars.s4, "0.", 0);
                }

                if (osd_vars.telemetry_lon_base > 9999999) {
                  if (numOfChars(osd_vars.s4) == 8) {
                    insertString(osd_vars.s4, ".", 1);
                  } else {
                    insertString(osd_vars.s4, ".", 2);
                  }
                }

                osd_vars.s1_double = strtod(osd_vars.s1, &osd_vars.ptr);
                osd_vars.s2_double = strtod(osd_vars.s2, &osd_vars.ptr);
                osd_vars.s3_double = strtod(osd_vars.s3, &osd_vars.ptr);
                osd_vars.s4_double = strtod(osd_vars.s4, &osd_vars.ptr);
              }
              osd_vars.telemetry_distance = distanceEarth(osd_vars.s1_double, osd_vars.s2_double, osd_vars.s3_double, osd_vars.s4_double);
            }
            break;

          case MAVLINK_MSG_ID_VFR_HUD:
            {
              mavlink_vfr_hud_t vfr;
              mavlink_msg_vfr_hud_decode(&message, &vfr);
              osd_vars.telemetry_gspeed = vfr.groundspeed * 3.6;
              osd_vars.telemetry_vspeed = vfr.climb;
              osd_vars.telemetry_altitude = vfr.alt;
            }
            break;

          case MAVLINK_MSG_ID_GLOBAL_POSITION_INT:
            {
              mavlink_global_position_int_t global_position_int;
              mavlink_msg_global_position_int_decode( &message, &global_position_int);
              osd_vars.telemetry_hdg = global_position_int.hdg / 100;
            }
            break;

          case MAVLINK_MSG_ID_ATTITUDE:
            {
              mavlink_attitude_t att;
              mavlink_msg_attitude_decode(&message, &att);
              osd_vars.telemetry_pitch = att.pitch * (180.0 / 3.141592653589793238463);
              osd_vars.telemetry_roll = att.roll * (180.0 / 3.141592653589793238463);
              osd_vars.telemetry_yaw = att.yaw * (180.0 / 3.141592653589793238463);
            }
            break;

          case MAVLINK_MSG_ID_RADIO_STATUS:
            {
                if ((message.sysid != 3) || (message.compid != 68)) {
                    break;
                }

                osd_vars.wfb_rssi = (int8_t)mavlink_msg_radio_status_get_rssi(&message);
                osd_vars.wfb_errors = mavlink_msg_radio_status_get_rxerrors(&message);
                osd_vars.wfb_fec_fixed = mavlink_msg_radio_status_get_fixed(&message);
                osd_vars.wfb_flags = mavlink_msg_radio_status_get_remnoise(&message);

                // printf("wfb_rssi=%d, wfb_errors=%d, wfb_fec_fixed=%d, wfb_flags=%d\n", osd_vars.wfb_rssi, osd_vars.wfb_errors, osd_vars.wfb_fec_fixed, osd_vars.wfb_flags);
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
