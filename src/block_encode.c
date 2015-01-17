/*
 * Centaurean Density
 *
 * Copyright (c) 2013, Guillaume Voirin
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Centaurean nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * 18/10/13 00:03
 */

#include "block_encode.h"
#include "density_api_data_structures.h"

DENSITY_FORCE_INLINE DENSITY_BLOCK_ENCODE_STATE density_block_encode_write_block_header(density_memory_location *restrict out, density_block_encode_state *restrict state) {
    if (sizeof(density_block_header) > out->available_bytes)
        return DENSITY_BLOCK_ENCODE_STATE_STALL_ON_OUTPUT;

    state->currentMode = state->targetMode;

    state->currentBlockData.inStart = state->totalRead;
    state->currentBlockData.outStart = state->totalWritten;

    state->totalWritten += density_block_header_write(out);

    state->process = DENSITY_BLOCK_ENCODE_PROCESS_WRITE_DATA;

    return DENSITY_BLOCK_ENCODE_STATE_READY;
}

DENSITY_FORCE_INLINE DENSITY_BLOCK_ENCODE_STATE density_block_encode_write_block_footer(density_memory_location *restrict out, density_block_encode_state *restrict state) {
    if (sizeof(density_block_footer) > out->available_bytes)
        return DENSITY_BLOCK_ENCODE_STATE_STALL_ON_OUTPUT;

    state->totalWritten += density_block_footer_write(out, 0);

    return DENSITY_BLOCK_ENCODE_STATE_READY;
}

DENSITY_FORCE_INLINE DENSITY_BLOCK_ENCODE_STATE density_block_encode_write_mode_marker(density_memory_location *restrict out, density_block_encode_state *restrict state) {
    if (sizeof(density_mode_marker) > out->available_bytes)
        return DENSITY_BLOCK_ENCODE_STATE_STALL_ON_OUTPUT;

    switch (state->blockMode) {
        case DENSITY_BLOCK_MODE_COPY:
            break;

        default:
            if (state->totalWritten > state->totalRead)
                state->blockMode = DENSITY_BLOCK_MODE_COPY;
            break;
    }

    state->totalWritten += density_block_mode_marker_write(out, state->blockMode);

    state->process = DENSITY_BLOCK_ENCODE_PROCESS_WRITE_DATA;

    return DENSITY_BLOCK_ENCODE_STATE_READY;
}

DENSITY_FORCE_INLINE void density_block_encode_update_totals(density_memory_teleport *restrict in, density_memory_location *restrict out, density_block_encode_state *restrict state, const uint_fast64_t availableInBefore, const uint_fast64_t availableOutBefore) {
    state->totalRead += availableInBefore - density_memory_teleport_available(in);
    state->totalWritten += availableOutBefore - out->available_bytes;
}

DENSITY_FORCE_INLINE DENSITY_BLOCK_ENCODE_STATE density_block_encode_init(density_block_encode_state *restrict state, const DENSITY_COMPRESSION_MODE mode, const DENSITY_BLOCK_TYPE blockType, void *kernelState, DENSITY_KERNEL_ENCODE_STATE (*kernelInit)(void *), DENSITY_KERNEL_ENCODE_STATE (*kernelProcess)(density_memory_teleport *, density_memory_location *, void *), DENSITY_KERNEL_ENCODE_STATE (*kernelFinish)(density_memory_teleport *, density_memory_location *, void *)) {
    state->process = DENSITY_BLOCK_ENCODE_PROCESS_WRITE_BLOCK_HEADER;
    state->blockType = blockType;
    state->targetMode = mode;
    state->currentMode = mode;
    state->blockMode = mode ? DENSITY_BLOCK_MODE_KERNEL : DENSITY_BLOCK_MODE_COPY;

    state->totalRead = 0;
    state->totalWritten = 0;

    switch (state->blockMode) {
        case DENSITY_BLOCK_MODE_KERNEL:
            state->kernelEncodeState = kernelState;
            state->kernelEncodeInit = kernelInit;
            state->kernelEncodeProcess = kernelProcess;
            state->kernelEncodeFinish = kernelFinish;

            state->kernelEncodeInit(state->kernelEncodeState);
            break;
        default:
            break;
    }

    return DENSITY_BLOCK_ENCODE_STATE_READY;
}

DENSITY_FORCE_INLINE DENSITY_BLOCK_ENCODE_STATE density_block_encode_continue(density_memory_teleport *restrict in, density_memory_location *restrict out, density_block_encode_state *restrict state) {
    DENSITY_BLOCK_ENCODE_STATE encodeState;
    DENSITY_KERNEL_ENCODE_STATE kernelEncodeState;
    uint_fast64_t availableInBefore;
    uint_fast64_t availableOutBefore;
    uint_fast64_t blockRemaining;
    uint_fast64_t inRemaining;
    uint_fast64_t outRemaining;

    while (true) {
        switch (state->process) {
            case DENSITY_BLOCK_ENCODE_PROCESS_WRITE_BLOCK_HEADER:
                if ((encodeState = density_block_encode_write_block_header(out, state)))
                    return encodeState;
                break;

            case DENSITY_BLOCK_ENCODE_PROCESS_WRITE_BLOCK_MODE_MARKER:
                if ((encodeState = density_block_encode_write_mode_marker(out, state)))
                    return encodeState;
                break;

            case DENSITY_BLOCK_ENCODE_PROCESS_WRITE_DATA:
                availableInBefore = density_memory_teleport_available(in);
                availableOutBefore = out->available_bytes;

                switch (state->blockMode) {
                    case DENSITY_BLOCK_MODE_COPY:
                        blockRemaining = (uint_fast64_t) DENSITY_PREFERRED_COPY_BLOCK_SIZE - (state->totalRead - state->currentBlockData.inStart);
                        inRemaining = density_memory_teleport_available(in);
                        outRemaining = out->available_bytes;

                        if (inRemaining <= outRemaining) {
                            if (blockRemaining <= inRemaining)
                                goto copy_until_end_of_block;
                            else {
                                density_memory_teleport_copy(in, out, inRemaining);
                                density_block_encode_update_totals(in, out, state, availableInBefore, availableOutBefore);
                                return DENSITY_BLOCK_ENCODE_STATE_STALL_ON_INPUT;
                            }
                        } else {
                            if (blockRemaining <= outRemaining)
                                goto copy_until_end_of_block;
                            else {
                                density_memory_teleport_copy(in, out, outRemaining);
                                density_block_encode_update_totals(in, out, state, availableInBefore, availableOutBefore);
                                return DENSITY_BLOCK_ENCODE_STATE_STALL_ON_OUTPUT;
                            }
                        }

                    copy_until_end_of_block:
                        density_memory_teleport_copy(in, out, blockRemaining);
                        density_block_encode_update_totals(in, out, state, availableInBefore, availableOutBefore);
                        state->process = DENSITY_BLOCK_ENCODE_PROCESS_WRITE_BLOCK_FOOTER;
                        if (!density_memory_teleport_available(in)) {
                            return DENSITY_BLOCK_ENCODE_STATE_STALL_ON_INPUT;
                        } else if (!out->available_bytes)
                            return DENSITY_BLOCK_ENCODE_STATE_STALL_ON_OUTPUT;
                        break;

                    case DENSITY_BLOCK_MODE_KERNEL:
                        kernelEncodeState = state->kernelEncodeProcess(in, out, state->kernelEncodeState);
                        density_block_encode_update_totals(in, out, state, availableInBefore, availableOutBefore);

                        switch (kernelEncodeState) {
                            case DENSITY_KERNEL_ENCODE_STATE_STALL_ON_INPUT:
                                return DENSITY_BLOCK_ENCODE_STATE_STALL_ON_INPUT;

                            case DENSITY_KERNEL_ENCODE_STATE_STALL_ON_OUTPUT:
                                return DENSITY_BLOCK_ENCODE_STATE_STALL_ON_OUTPUT;

                            case DENSITY_KERNEL_ENCODE_STATE_INFO_NEW_BLOCK:
                                state->process = DENSITY_BLOCK_ENCODE_PROCESS_WRITE_BLOCK_FOOTER;
                                break;

                            case DENSITY_KERNEL_ENCODE_STATE_INFO_EFFICIENCY_CHECK:
                                state->process = DENSITY_BLOCK_ENCODE_PROCESS_WRITE_BLOCK_MODE_MARKER;
                                break;

                            case DENSITY_KERNEL_ENCODE_STATE_READY:
                                return DENSITY_BLOCK_ENCODE_STATE_READY;

                            default:
                                return DENSITY_BLOCK_ENCODE_STATE_ERROR;
                        }
                        break;

                    default:
                        return DENSITY_BLOCK_ENCODE_STATE_ERROR;
                }
                break;

            case DENSITY_BLOCK_ENCODE_PROCESS_WRITE_BLOCK_FOOTER:
                if (state->blockType == DENSITY_BLOCK_TYPE_DEFAULT) if ((encodeState = density_block_encode_write_block_footer(out, state)))
                    return encodeState;
                state->process = DENSITY_BLOCK_ENCODE_PROCESS_WRITE_BLOCK_HEADER;
                break;

            default:
                return DENSITY_BLOCK_ENCODE_STATE_ERROR;
        }
    }
}

DENSITY_FORCE_INLINE DENSITY_BLOCK_ENCODE_STATE density_block_encode_finish(density_memory_teleport *restrict in, density_memory_location *restrict out, density_block_encode_state *restrict state) {
    DENSITY_BLOCK_ENCODE_STATE encodeState;
    DENSITY_KERNEL_ENCODE_STATE kernelEncodeState;
    uint_fast64_t availableInBefore;
    uint_fast64_t availableOutBefore;
    uint_fast64_t blockRemaining;
    uint_fast64_t inRemaining;
    uint_fast64_t outRemaining;

    while (true) {
        switch (state->process) {
            case DENSITY_BLOCK_ENCODE_PROCESS_WRITE_BLOCK_HEADER:
                if ((encodeState = density_block_encode_write_block_header(out, state)))
                    return encodeState;
                break;

            case DENSITY_BLOCK_ENCODE_PROCESS_WRITE_BLOCK_MODE_MARKER:
                if ((encodeState = density_block_encode_write_mode_marker(out, state)))
                    return encodeState;
                break;

            case DENSITY_BLOCK_ENCODE_PROCESS_WRITE_DATA:
                availableInBefore = density_memory_teleport_available(in);
                availableOutBefore = out->available_bytes;

                switch (state->blockMode) {
                    case DENSITY_BLOCK_MODE_COPY:
                        blockRemaining = (uint_fast64_t) DENSITY_PREFERRED_COPY_BLOCK_SIZE - (state->totalRead - state->currentBlockData.inStart);
                        inRemaining = density_memory_teleport_available(in);
                        outRemaining = out->available_bytes;

                        if (inRemaining <= outRemaining) {
                            if (blockRemaining <= inRemaining)
                                goto copy_until_end_of_block;
                            else {
                                density_memory_teleport_copy(in, out, inRemaining);
                                density_block_encode_update_totals(in, out, state, availableInBefore, availableOutBefore);
                                goto write_block_footer;
                            }
                        } else {
                            if (blockRemaining <= outRemaining)
                                goto copy_until_end_of_block;
                            else {
                                density_memory_teleport_copy(in, out, outRemaining);
                                density_block_encode_update_totals(in, out, state, availableInBefore, availableOutBefore);
                                return DENSITY_BLOCK_ENCODE_STATE_STALL_ON_OUTPUT;
                            }
                        }

                    copy_until_end_of_block:
                        density_memory_teleport_copy(in, out, blockRemaining);
                        density_block_encode_update_totals(in, out, state, availableInBefore, availableOutBefore);
                    write_block_footer:
                        state->process = DENSITY_BLOCK_ENCODE_PROCESS_WRITE_BLOCK_FOOTER;
                        if (!density_memory_teleport_available(in)) {
                            break;
                        } else if (!out->available_bytes)
                            return DENSITY_BLOCK_ENCODE_STATE_STALL_ON_OUTPUT;
                        break;

                    case DENSITY_BLOCK_MODE_KERNEL:
                        kernelEncodeState = state->kernelEncodeFinish(in, out, state->kernelEncodeState);
                        density_block_encode_update_totals(in, out, state, availableInBefore, availableOutBefore);

                        switch (kernelEncodeState) {
                            case DENSITY_KERNEL_ENCODE_STATE_STALL_ON_INPUT:
                                return DENSITY_BLOCK_ENCODE_STATE_ERROR;

                            case DENSITY_KERNEL_ENCODE_STATE_STALL_ON_OUTPUT:
                                return DENSITY_BLOCK_ENCODE_STATE_STALL_ON_OUTPUT;

                            case DENSITY_KERNEL_ENCODE_STATE_INFO_NEW_BLOCK:
                                state->process = DENSITY_BLOCK_ENCODE_PROCESS_WRITE_BLOCK_FOOTER;
                                break;

                            case DENSITY_KERNEL_ENCODE_STATE_INFO_EFFICIENCY_CHECK:
                                state->process = DENSITY_BLOCK_ENCODE_PROCESS_WRITE_BLOCK_MODE_MARKER;
                                break;

                            case DENSITY_KERNEL_ENCODE_STATE_READY:
                                state->process = DENSITY_BLOCK_ENCODE_PROCESS_WRITE_BLOCK_FOOTER;
                                break;

                            default:
                                return DENSITY_BLOCK_ENCODE_STATE_ERROR;
                        }
                        break;
                }

            case DENSITY_BLOCK_ENCODE_PROCESS_WRITE_BLOCK_FOOTER:
                if (state->blockType == DENSITY_BLOCK_TYPE_WITH_HASHSUM_INTEGRITY_CHECK) if ((encodeState = density_block_encode_write_block_footer(out, state)))
                    return encodeState;
                state->process = DENSITY_BLOCK_ENCODE_PROCESS_WRITE_BLOCK_HEADER;
                if (!density_memory_teleport_available(in))
                    return DENSITY_BLOCK_ENCODE_STATE_READY;
                break;

            default:
                return DENSITY_BLOCK_ENCODE_STATE_ERROR;
        }
    }
}