#ifndef RTP_FRAME_H
#define RTP_FRAME_H

#include <stdint.h>

typedef struct {
    uint8_t version: 2;    // RTP version
    uint8_t padding: 1;    // Padding flag
    uint8_t extension: 1;  // Extension flag
    uint8_t csrc_count: 4; // CSRC count
    uint8_t marker: 1;     // Marker bit
    uint8_t payload_type: 7; // Payload type
    uint16_t sequence_number; // Sequence number
    uint32_t timestamp;     // Timestamp
    uint32_t ssrc;          // Synchronization source identifier
} rtp_header_t;

// RTP frame handling.

uint8_t* decode_frame(uint8_t* rx_buffer, uint32_t rx_size, uint32_t header_size, uint8_t* nal_buffer, uint32_t* out_nal_size);

uint16_t rtp_sequence(const rtp_header_t *header);

#endif
