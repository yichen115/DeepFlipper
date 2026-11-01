#include "firmware/flipper/applications/services/expansion/expansion_protocol.h"
#include <string.h>

typedef struct {
    const uint8_t* input;
    size_t input_size, position;
    uint8_t* output;
    size_t output_size, capacity;
} ReferenceWire;
static size_t read_wire(uint8_t* data, size_t size, void* context) {
    ReferenceWire* wire = context;
    size_t remaining = wire->input_size - wire->position;
    if (size > remaining) size = remaining;
    if (size) memcpy(data, wire->input + wire->position, size);
    wire->position += size;
    return size;
}
static size_t write_wire(const uint8_t* data, size_t size, void* context) {
    ReferenceWire* wire = context;
    if (size > wire->capacity - wire->output_size) return 0;
    memcpy(wire->output + wire->output_size, data, size);
    wire->output_size += size;
    return size;
}
int expansion_reference_roundtrip(const uint8_t* input, size_t input_size,
    uint8_t* output, size_t* output_size, uint8_t* type, uint32_t* baud, uint8_t* data_size) {
    ReferenceWire wire = {.input=input, .input_size=input_size, .output=output, .capacity=*output_size};
    ExpansionFrame frame = {0};
    ExpansionProtocolStatus status = expansion_protocol_decode(&frame,read_wire,&wire);
    if (status != ExpansionProtocolStatusOk) return status;
    if (wire.position != wire.input_size) return ExpansionProtocolStatusErrorFormat;
    *type = frame.header.type;
    *baud = frame.header.type == ExpansionFrameTypeBaudRate ? frame.content.baud_rate.baud : 0;
    *data_size = frame.header.type == ExpansionFrameTypeData ? frame.content.data.size : 0;
    status = expansion_protocol_encode(&frame,write_wire,&wire);
    *output_size = wire.output_size;
    return status;
}
