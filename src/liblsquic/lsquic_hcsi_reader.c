/* Copyright (c) 2017 - 2019 LiteSpeed Technologies Inc.  See LICENSE. */
#include <assert.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/queue.h>

#include "lsquic.h"
#include "lsquic_int_types.h"
#include "lsquic_varint.h"
#include "lsquic_hq.h"
#include "lsquic_hash.h"
#include "lsquic_conn.h"
#include "lsquic_hcsi_reader.h"

#define LSQUIC_LOGGER_MODULE LSQLM_HCSI_READER
#define LSQUIC_LOG_CONN_ID lsquic_conn_log_cid(reader->hr_conn)
#include "lsquic_logger.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))


void
lsquic_hcsi_reader_init (struct hcsi_reader *reader,
        struct lsquic_conn *conn, const struct hcsi_callbacks *callbacks,
        void *ctx)
{
    memset(reader, 0, sizeof(*reader));
    reader->hr_state = HR_READ_FRAME_LENGTH;
    reader->hr_conn = conn;
    reader->hr_cb = callbacks;
    reader->hr_ctx = ctx;
    LSQ_DEBUG("initialized");
}


int
lsquic_hcsi_reader_feed (struct hcsi_reader *reader, const void *buf,
                                                                size_t bufsz)
{
    const unsigned char *p = buf;
    const unsigned char *const end = p + bufsz;

    const unsigned char *orig_p;
    enum h3_prio_frame_read_status prio_status;
    uint64_t len;
    int s;

    while (p < end)
    {
        switch (reader->hr_state)
        {
        case HR_READ_FRAME_LENGTH:
            reader->hr_varint_state.pos = 0;
            reader->hr_state = HR_READ_FRAME_LENGTH_CONTINUE;
            /* fall-through */
        case HR_READ_FRAME_LENGTH_CONTINUE:
            s = lsquic_varint_read_nb(&p, end, &reader->hr_varint_state);
            if (0 == s)
            {
                reader->hr_frame_length = reader->hr_varint_state.val;
                reader->hr_state = HR_READ_FRAME_TYPE;
                break;
            }
            else
            {
                assert(p == end);
                return 0;
            }
        case HR_READ_VARINT:
            reader->hr_varint_state.pos = 0;
            reader->hr_state = HR_READ_VARINT_CONTINUE;
            reader->hr_nread = 0;
            /* fall-through */
        case HR_READ_VARINT_CONTINUE:
            orig_p = p;
            s = lsquic_varint_read_nb(&p, end, &reader->hr_varint_state);
            reader->hr_nread += p - orig_p;
            if (0 == s)
            {
                if (reader->hr_nread != reader->hr_frame_length)
                {
                    reader->hr_conn->cn_if->ci_abort_error(reader->hr_conn, 1,
                        HEC_MALFORMED_FRAME + reader->hr_frame_type,
                        "Frame length does not match actual payload length");
                    reader->hr_state = HR_ERROR;
                    return -1;
                }
                switch (reader->hr_frame_type)
                {
                case HQFT_GOAWAY:
                    reader->hr_cb->on_goaway(reader->hr_ctx,
                                                reader->hr_varint_state.val);
                    break;
                case HQFT_CANCEL_PUSH:
                    reader->hr_cb->on_cancel_push(reader->hr_ctx,
                                                reader->hr_varint_state.val);
                    break;
                case HQFT_MAX_PUSH_ID:
                    reader->hr_cb->on_max_push_id(reader->hr_ctx,
                                                reader->hr_varint_state.val);
                    break;
                default:
                    assert(0);
                }
                reader->hr_state = HR_READ_FRAME_LENGTH;
                break;
            }
            else
            {
                assert(p == end);
                return 0;
            }
        case HR_READ_FRAME_TYPE:
            reader->hr_frame_type = *p++;
            switch (reader->hr_frame_type)
            {
            case HQFT_SETTINGS:
                if (reader->hr_frame_length)
                {
                    reader->hr_state = HR_READ_SETTING_ID;
                    reader->hr_nread = 0;
                }
                else
                {
                    reader->hr_cb->on_settings_frame(reader->hr_ctx);
                    reader->hr_state = HR_READ_FRAME_LENGTH;
                }
                break;
            case HQFT_PRIORITY:
                reader->hr_state = HR_READ_PRIORITY_BEGIN;
                break;
            case HQFT_GOAWAY:
                reader->hr_state = HR_READ_VARINT;
                break;
            case HQFT_CANCEL_PUSH:
                reader->hr_state = HR_READ_VARINT;
                break;
            case HQFT_MAX_PUSH_ID:
                reader->hr_state = HR_READ_VARINT;
                break;
            case HQFT_DATA:
            case HQFT_HEADERS:
            case HQFT_PUSH_PROMISE:
                reader->hr_cb->on_unexpected_frame(reader->hr_ctx,
                                                        reader->hr_frame_type);
                return -1;
            default:
                if (!(reader->hr_frame_type >= 0xB &&
                        (reader->hr_frame_type - 0xB) % 0x1F == 0))
                    LSQ_INFO("unknown frame type 0x%X -- skipping",
                                                        reader->hr_frame_type);
                reader->hr_state = HR_SKIPPING;
                LSQ_DEBUG("unknown frame 0x%X: will skip %"PRIu64" bytes",
                            reader->hr_frame_type, reader->hr_frame_length);
                break;
            }
            break;
        case HR_SKIPPING:
            len = MIN((uintptr_t) (end - p), reader->hr_frame_length);
            p += len;
            reader->hr_frame_length -= len;
            if (0 == reader->hr_frame_length)
                reader->hr_state = HR_READ_FRAME_LENGTH;
            break;
        case HR_READ_SETTING_ID:
            reader->hr_u.settings.id = *p++ << 8;
            reader->hr_nread++;
            reader->hr_state = HR_READ_SETTING_ID_CONTINUE;
            break;
        case HR_READ_SETTING_ID_CONTINUE:
            reader->hr_u.settings.id |= *p++;
            reader->hr_nread++;
            reader->hr_state = HR_READ_SETTING_VALUE;
            break;
        case HR_READ_SETTING_VALUE:
            reader->hr_varint_state.pos = 0;
            reader->hr_state = HR_READ_SETTING_VALUE_CONTINUE;
            /* fall-through */
        case HR_READ_SETTING_VALUE_CONTINUE:
            orig_p = p;
            s = lsquic_varint_read_nb(&p, end, &reader->hr_varint_state);
            reader->hr_nread += p - orig_p;
            if (reader->hr_nread > reader->hr_frame_length)
            {
                reader->hr_conn->cn_if->ci_abort_error(reader->hr_conn, 1,
                    HEC_MALFORMED_FRAME + HQFT_SETTINGS,
                    "SETTING frame contents too long");
                reader->hr_state = HR_ERROR;
                return -1;
            }
            if (0 == s)
            {
                reader->hr_cb->on_setting(reader->hr_ctx,
                        reader->hr_u.settings.id, reader->hr_varint_state.val);
                if (reader->hr_nread >= reader->hr_frame_length)
                {
                    reader->hr_state = HR_READ_FRAME_LENGTH;
                    reader->hr_cb->on_settings_frame(reader->hr_ctx);
                }
                else
                    reader->hr_state = HR_READ_SETTING_ID;
                break;
            }
            else
            {
                assert(p == end);
                return 0;
            }
        case HR_READ_PRIORITY_BEGIN:
            reader->hr_u.prio.h3pfrs_state = 0;
            reader->hr_nread = 0;
            reader->hr_state = HR_READ_PRIORITY_CONTINUE;
            /* fall-through */
        case HR_READ_PRIORITY_CONTINUE:
            orig_p = p;
            prio_status = lsquic_h3_prio_frame_read(&p, end - p,
                                                        &reader->hr_u.prio);
            reader->hr_nread += p - orig_p;
            if (prio_status == H3PFR_STATUS_DONE)
            {
                if (reader->hr_nread != reader->hr_frame_length)
                {
                    reader->hr_conn->cn_if->ci_abort_error(reader->hr_conn, 1,
                        HEC_MALFORMED_FRAME + HQFT_PRIORITY, "PRIORITY frame "
                        "contents size does not match frame length");
                    reader->hr_state = HR_ERROR;
                    return -1;
                }
                reader->hr_state = HR_READ_FRAME_LENGTH;
                reader->hr_cb->on_priority(reader->hr_ctx,
                                                    &reader->hr_u.prio.h3pfrs_prio);
            }
            break;
        default:
            assert(0);
            /* fall-through */
        case HR_ERROR:
            return -1;
        }
    }

    return 0;
}