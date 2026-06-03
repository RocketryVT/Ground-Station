#pragma once

#include "shared.hpp"

#include "pb_encode.h"
#include "ground_station.pb.h"

#include <string.h>

static inline bool mqtt_encode_proto( MqttMessage& out,
                                      const char* topic,
                                      const pb_msgdesc_t* fields,
                                      const void* message )
{
    memset( &out, 0, sizeof(out) );
    strncpy( out.topic, topic, sizeof(out.topic) - 1 );

    pb_ostream_t stream = pb_ostream_from_buffer( out.payload, sizeof(out.payload) );
    if ( !pb_encode( &stream, fields, message ) ) {
        out.payload_len = 0;
        return false;
    }

    out.payload_len = (uint16_t)stream.bytes_written;
    return true;
}

static inline bool mqtt_set_binary( MqttMessage& out,
                                    const char* topic,
                                    const uint8_t* payload,
                                    uint16_t len )
{
    if ( len > sizeof(out.payload) ) return false;

    memset( &out, 0, sizeof(out) );
    strncpy( out.topic, topic, sizeof(out.topic) - 1 );
    memcpy( out.payload, payload, len );
    out.payload_len = len;
    return true;
}
