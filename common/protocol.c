#include "protocol.h"
#include <string.h>
#include <arpa/inet.h>

/* ─── tlv_encode ───────────────────────────────────────────────────────────
   Writes a single TLV unit into buf:
     buf[0]        = tag
     buf[1..2]     = value_len in big endian (network byte order)
     buf[3..3+len] = value bytes

   Returns total bytes written, or -1 if buf is too small.
────────────────────────────────────────────────────────────────────────── */
int tlv_encode(uint8_t *buf, size_t buf_size, uint8_t tag,
               const uint8_t *value, uint16_t value_len)
{
    if (buf_size < (size_t)(TLV_HEADER_SIZE + value_len))
    {
        return -1;
    }

    uint16_t net_len = htons(value_len);

    buf[0] = tag;
    memcpy(buf + TLV_TAG_SIZE, &net_len, TLV_LENGTH_SIZE);

    if (value_len > 0)
    {
        memcpy(buf + TLV_HEADER_SIZE, value, value_len);
    }

    return TLV_HEADER_SIZE + value_len;
}

/* ─── tlv_decode ───────────────────────────────────────────────────────────
   Reads one TLV unit from buf.
   Sets out_tag, out_value (pointer into buf — no copy), out_value_len.
   Returns total bytes consumed, or -1 if buf is too short / malformed.
────────────────────────────────────────────────────────────────────────── */
int tlv_decode(const uint8_t *buf, size_t buf_len,
               uint8_t *out_tag, const uint8_t **out_value,
               uint16_t *out_value_len)
{
    if (buf_len < TLV_HEADER_SIZE)
    {
        return -1;
    }

    uint8_t tag = buf[0];
    uint16_t net_len;
    memcpy(&net_len, buf + TLV_TAG_SIZE, TLV_LENGTH_SIZE);
    uint16_t value_len = ntohs(net_len);  /* big endian → host */

    if (buf_len < (size_t)(TLV_HEADER_SIZE + value_len))
    {
        return -1;
    }

    *out_tag = tag;
    *out_value = buf + TLV_HEADER_SIZE; /* point into buf, no copy */
    *out_value_len = value_len;

    return TLV_HEADER_SIZE + value_len;
}

/* ─── tlv_find_field ───────────────────────────────────────────────────────
   Walks a value block (the inner bytes of a message) TLV by TLV,
   searching for field_tag.
   On success: sets out_value and out_value_len, returns PROTOCOL_SUCCESS.
   Returns PROTOCOL_MALFORMED or PROTOCOL_FIELD_NOT_FOUND on failure.
────────────────────────────────────────────────────────────────────────── */
ProtocolResult tlv_find_field(const uint8_t *value_block, uint16_t block_len,
                               uint8_t field_tag, const uint8_t **out_value,
                               uint16_t *out_value_len)
{
    size_t offset = 0;

    while (offset < block_len)
    {
        uint8_t tag;
        const uint8_t *value;
        uint16_t value_len;

        int consumed = tlv_decode(value_block + offset, block_len - offset,
                                  &tag, &value, &value_len);
        if (consumed < 0)
        {
            return PROTOCOL_MALFORMED;
        }

        if (tag == field_tag)
        {
            *out_value = value;
            *out_value_len = value_len;
            return PROTOCOL_SUCCESS;
        }

        offset += consumed;
    }

    return PROTOCOL_FIELD_NOT_FOUND;
}
