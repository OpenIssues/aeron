/*
 * Copyright 2014-2020 Real Logic Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <errno.h>
#include <inttypes.h>

#include "aeronc.h"
#include "aeron_common.h"
#include "aeron_publication.h"
#include "aeron_alloc.h"
#include "util/aeron_error.h"
#include "util/aeron_fileutil.h"
#include "concurrent/aeron_counters_manager.h"
#include "concurrent/aeron_term_appender.h"

int aeron_publication_create(
    aeron_publication_t **publication,
    aeron_client_conductor_t *conductor,
    const char *channel,
    int32_t stream_id,
    int32_t session_id,
    int32_t position_limit_id,
    int32_t channel_status_id,
    const char *log_file,
    int64_t original_registration_id,
    int64_t registration_id,
    bool pre_touch)
{
    aeron_publication_t *_publication;

    *publication = NULL;
    if (aeron_alloc((void **)&_publication, sizeof(aeron_publication_t)) < 0)
    {
        int errcode = errno;

        aeron_set_err(errcode, "aeron_publication_create (%d): %s", errcode, strerror(errcode));
        return -1;
    }

    if (aeron_map_existing_log(&_publication->mapped_raw_log, log_file, pre_touch) < 0)
    {
        aeron_free(_publication);
        return -1;
    }

    _publication->log_meta_data = (aeron_logbuffer_metadata_t *)_publication->mapped_raw_log.log_meta_data.addr;

    _publication->conductor = conductor;
    _publication->channel = channel;
    _publication->registration_id = registration_id;
    _publication->original_registration_id = original_registration_id;
    _publication->stream_id = stream_id;
    _publication->session_id = session_id;
    _publication->is_closed = false;

    *publication = _publication;
    return -1;
}

int aeron_publication_delete(aeron_publication_t *publication)
{
    aeron_map_raw_log_close(&publication->mapped_raw_log, NULL);
    aeron_free((void *)publication->channel);
    aeron_free(publication);

    return 0;
}

int aeron_publication_close(aeron_publication_t *publication)
{
    return NULL != publication ?
        aeron_client_conductor_async_close_publication(publication->conductor, publication) : 0;
}

int64_t aeron_publication_offer(
    aeron_publication_t *publication,
    uint8_t *buffer,
    size_t length,
    aeron_reserved_value_supplier_t reserved_value_supplier,
    void *clientd)
{
    int64_t new_position = AERON_PUBLICATION_CLOSED;
    bool is_closed;

    if (NULL == publication || buffer == NULL)
    {
        errno = EINVAL;
        aeron_set_err(EINVAL, "aeron_publication_offer(NULL): %s", strerror(EINVAL));
        return AERON_PUBLICATION_ERROR;
    }

    AERON_GET_VOLATILE(is_closed, publication->is_closed);
    if (!is_closed)
    {
        const int64_t limit = aeron_counter_get_volatile(publication->position_limit);
        const int32_t term_count = aeron_logbuffer_active_term_count(publication->log_meta_data);
        const size_t index = aeron_logbuffer_index_by_term_count(term_count);
        const int64_t raw_tail = aeron_term_appender_raw_tail_volatile(
            &publication->log_meta_data->term_tail_counters[index]);
        const int64_t term_offset = raw_tail & 0xFFFFFFFF;
        const int32_t term_id = aeron_logbuffer_term_id(raw_tail);
        const int64_t position = aeron_logbuffer_compute_term_begin_position(
            term_id, publication->position_bits_to_shift, publication->initial_term_id);

        if (term_count != (term_id - publication->initial_term_id))
        {
            return AERON_PUBLICATION_ADMIN_ACTION;
        }

        if (position < limit)
        {
            int32_t resulting_offset;
            if (length <= publication->max_payload_length)
            {
                resulting_offset = aeron_term_appender_append_unfragmented_message(
                    &publication->mapped_raw_log.term_buffers[index],
                    &publication->log_meta_data->term_tail_counters[index],
                    buffer,
                    length,
                    reserved_value_supplier,
                    clientd,
                    term_id,
                    publication->session_id,
                    publication->stream_id);
            }
            else
            {
                if (length > publication->max_message_length)
                {
                    errno = EINVAL;
                    aeron_set_err(EINVAL, "aeron_publication_offer: length=%" PRIu32 " > max_message_length=%" PRIu32,
                        (uint32_t)length, (uint32_t)publication->max_message_length);
                    return AERON_PUBLICATION_ERROR;
                }

                resulting_offset = aeron_term_appender_append_fragmented_message(
                    &publication->mapped_raw_log.term_buffers[index],
                    &publication->log_meta_data->term_tail_counters[index],
                    buffer,
                    length,
                    publication->max_payload_length,
                    reserved_value_supplier,
                    clientd,
                    term_id,
                    publication->session_id,
                    publication->stream_id);
            }

            new_position = aeron_publication_new_position(
                publication, term_count, (int32_t)term_offset, term_id, position, resulting_offset);
        }
        else
        {
            new_position = aeron_publication_back_pressure_status(publication, position, (int32_t)length);
        }
    }

    return new_position;
}

int64_t aeron_publication_offerv(
    aeron_publication_t *publication,
    aeron_iovec_t *iov,
    size_t iovcnt,
    aeron_reserved_value_supplier_t reserved_value_supplier,
    void *clientd)
{
    int64_t new_position = AERON_PUBLICATION_CLOSED;
    bool is_closed;

    if (NULL == publication || iov == NULL)
    {
        errno = EINVAL;
        aeron_set_err(EINVAL, "aeron_publication_offerv(NULL): %s", strerror(EINVAL));
        return AERON_PUBLICATION_ERROR;
    }

    size_t length = 0;
    for (size_t i = 0; i < iovcnt; i++)
    {
        length += iov[i].iov_len;
    }

    AERON_GET_VOLATILE(is_closed, publication->is_closed);
    if (!is_closed)
    {
        const int64_t limit = aeron_counter_get_volatile(publication->position_limit);
        const int32_t term_count = aeron_logbuffer_active_term_count(publication->log_meta_data);
        const size_t index = aeron_logbuffer_index_by_term_count(term_count);
        const int64_t raw_tail = aeron_term_appender_raw_tail_volatile(
            &publication->log_meta_data->term_tail_counters[index]);
        const int64_t term_offset = raw_tail & 0xFFFFFFFF;
        const int32_t term_id = aeron_logbuffer_term_id(raw_tail);
        const int64_t position = aeron_logbuffer_compute_term_begin_position(
            term_id, publication->position_bits_to_shift, publication->initial_term_id);

        if (term_count != (term_id - publication->initial_term_id))
        {
            return AERON_PUBLICATION_ADMIN_ACTION;
        }

        if (position < limit)
        {
            int32_t resulting_offset;
            if (length <= publication->max_payload_length)
            {
                resulting_offset = aeron_term_appender_append_unfragmented_messagev(
                    &publication->mapped_raw_log.term_buffers[index],
                    &publication->log_meta_data->term_tail_counters[index],
                    iov,
                    iovcnt,
                    length,
                    reserved_value_supplier,
                    clientd,
                    term_id,
                    publication->session_id,
                    publication->stream_id);
            }
            else
            {
                if (length > publication->max_message_length)
                {
                    errno = EINVAL;
                    aeron_set_err(EINVAL, "aeron_publication_offerv: length=%" PRIu32 " > max_message_length=%" PRIu32,
                        (uint32_t)length, (uint32_t)publication->max_message_length);
                    return AERON_PUBLICATION_ERROR;
                }

                resulting_offset = aeron_term_appender_append_fragmented_messagev(
                    &publication->mapped_raw_log.term_buffers[index],
                    &publication->log_meta_data->term_tail_counters[index],
                    iov,
                    iovcnt,
                    length,
                    publication->max_payload_length,
                    reserved_value_supplier,
                    clientd,
                    term_id,
                    publication->session_id,
                    publication->stream_id);
            }

            new_position = aeron_publication_new_position(
            publication, term_count, (int32_t)term_offset, term_id, position, resulting_offset);
        }
        else
        {
            new_position = aeron_publication_back_pressure_status(publication, position, (int32_t)length);
        }
    }

    return new_position;
}

int64_t aeron_publication_try_claim(
    aeron_publication_t *publication,
    size_t length,
    aeron_buffer_claim_t *buffer_claim)
{
    int64_t new_position = AERON_PUBLICATION_CLOSED;
    bool is_closed;

    if (NULL == publication || buffer_claim == NULL)
    {
        errno = EINVAL;
        aeron_set_err(EINVAL, "aeron_publication_try_claim(NULL): %s", strerror(EINVAL));
        return AERON_PUBLICATION_ERROR;
    }
    else if (length > publication->max_payload_length)
    {
        errno = EINVAL;
        aeron_set_err(EINVAL, "aeron_publication_try_claim: length=%" PRIu32 " > max_payload_length=%" PRIu32,
            (uint32_t)length, (uint32_t)publication->max_payload_length);
        return AERON_PUBLICATION_ERROR;
    }

    AERON_GET_VOLATILE(is_closed, publication->is_closed);
    if (!is_closed)
    {
        const int64_t limit = aeron_counter_get_volatile(publication->position_limit);
        const int32_t term_count = aeron_logbuffer_active_term_count(publication->log_meta_data);
        const size_t index = aeron_logbuffer_index_by_term_count(term_count);
        const int64_t raw_tail = aeron_term_appender_raw_tail_volatile(
            &publication->log_meta_data->term_tail_counters[index]);
        const int64_t term_offset = raw_tail & 0xFFFFFFFF;
        const int32_t term_id = aeron_logbuffer_term_id(raw_tail);
        const int64_t position = aeron_logbuffer_compute_term_begin_position(
            term_id, publication->position_bits_to_shift, publication->initial_term_id);

        if (term_count != (term_id - publication->initial_term_id))
        {
            return AERON_PUBLICATION_ADMIN_ACTION;
        }

        if (position < limit)
        {
            int32_t resulting_offset = aeron_term_appender_claim(
                    &publication->mapped_raw_log.term_buffers[index],
                    &publication->log_meta_data->term_tail_counters[index],
                    length,
                    buffer_claim,
                    term_id,
                    publication->session_id,
                    publication->stream_id);

            new_position = aeron_publication_new_position(
                publication, term_count, (int32_t)term_offset, term_id, position, resulting_offset);
        }
        else
        {
            new_position = aeron_publication_back_pressure_status(publication, position, (int32_t)length);
        }
    }

    return new_position;
}

extern int64_t aeron_publication_new_position(
    aeron_publication_t *publication,
    int32_t term_count,
    int32_t term_offset,
    int32_t term_id,
    int64_t position,
    int32_t resulting_offset);

extern int64_t aeron_publication_back_pressure_status(
    aeron_publication_t *publication, int64_t current_position, int32_t message_length);
