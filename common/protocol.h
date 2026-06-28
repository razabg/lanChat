#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/* ─── Message Tags ─────────────────────────────────────────────────────────
   Outer tags — identify the message type.
   Every message sent over TCP starts with one of these.
────────────────────────────────────────────────────────────────────────── */
#define TAG_REGISTER_REQ      0x01
#define TAG_REGISTER_RESP     0x02
#define TAG_LOGIN_REQ         0x03
#define TAG_LOGIN_RESP        0x04
#define TAG_LOGOUT_REQ        0x05
#define TAG_LOGOUT_RESP       0x06
#define TAG_CREATE_GROUP_REQ  0x07
#define TAG_CREATE_GROUP_RESP 0x08
#define TAG_JOIN_GROUP_REQ    0x09
#define TAG_JOIN_GROUP_RESP   0x0A
#define TAG_LEAVE_GROUP_REQ   0x0B
#define TAG_LEAVE_GROUP_RESP  0x0C

/* ─── Field Tags ───────────────────────────────────────────────────────────
   Inner tags — identify a specific field inside a message's value.
────────────────────────────────────────────────────────────────────────── */
#define TAG_USERNAME   0x20   /* UTF-8 string                        */
#define TAG_PASSWORD   0x21   /* UTF-8 string                        */
#define TAG_GROUP_NAME 0x22   /* UTF-8 string                        */
#define TAG_MC_IP      0x23   /* IPv4 string e.g. "239.0.0.1"        */
#define TAG_MC_PORT    0x24   /* 2 bytes, network byte order         */
#define TAG_STATUS     0x25   /* 1 byte status code (see StatusCode) */
#define TAG_ERROR_MSG  0x26   /* UTF-8 string                        */

/* ─── Status Codes ─────────────────────────────────────────────────────────
   Sent inside a TAG_STATUS field in every response.
   Using enum so the compiler warns on incomplete switch cases.
────────────────────────────────────────────────────────────────────────── */
typedef enum {
    STATUS_SUCCESS                 = 0,
    STATUS_USERNAME_ALREADY_EXISTS = 1,
    STATUS_USERNAME_NOT_FOUND      = 2,
    STATUS_WRONG_PASSWORD          = 3,
    STATUS_ALREADY_LOGGED_IN       = 4,
    STATUS_GROUP_ALREADY_EXISTS    = 5,
    STATUS_GROUP_NOT_FOUND         = 6,
    STATUS_ALREADY_IN_GROUP        = 7,
    STATUS_NOT_IN_GROUP            = 8,
    STATUS_SERVER_ERROR            = 9
} StatusCode;

/* ─── TLV Layout ───────────────────────────────────────────────────────────
   Wire format per TLV unit:
     [ tag: 1 byte ][ length: 2 bytes ][ value: length bytes ]
────────────────────────────────────────────────────────────────────────── */
#define TLV_TAG_SIZE    1
#define TLV_LENGTH_SIZE 2
#define TLV_HEADER_SIZE (TLV_TAG_SIZE + TLV_LENGTH_SIZE)  /* 3 bytes */

#define MAX_TLV_VALUE_SIZE 512
#define MAX_TLV_SIZE       (TLV_HEADER_SIZE + MAX_TLV_VALUE_SIZE)

/* ─── Protocol Result Codes ────────────────────────────────────────────────*/
typedef enum {
    PROTOCOL_SUCCESS,
    PROTOCOL_FIELD_NOT_FOUND,
    PROTOCOL_MALFORMED
} ProtocolResult;

/* ─── Function Signatures ──────────────────────────────────────────────────*/

/*
 * Encodes a single TLV into buf.
 * Returns the total bytes written (TLV_HEADER_SIZE + value_len),
 * or -1 if buf is too small.
 */
int tlv_encode(uint8_t *buf, size_t buf_size, uint8_t tag,
               const uint8_t *value, uint16_t value_len);

/*
 * Decodes one TLV from buf.
 * On success: fills out_tag, out_value (pointer into buf), out_value_len.
 * Returns total bytes consumed (header + value), or -1 on error.
 */
int tlv_decode(const uint8_t *buf, size_t buf_len,
               uint8_t *out_tag, const uint8_t **out_value,
               uint16_t *out_value_len);

/*
 * Searches a value block (the inner bytes of a message) for a field tag.
 * On success: sets out_value and out_value_len for that field.
 * Returns PROTOCOL_SUCCESS, PROTOCOL_FIELD_NOT_FOUND, or PROTOCOL_MALFORMED.
 */
ProtocolResult tlv_find_field(const uint8_t *value_block, uint16_t block_len,
                               uint8_t field_tag, const uint8_t **out_value,
                               uint16_t *out_value_len);


                   
#endif /* PROTOCOL_H */