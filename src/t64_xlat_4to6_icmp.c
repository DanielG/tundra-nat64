/*
Copyright (c) 2022 Vít Labuda. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
following conditions are met:
 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
    disclaimer.
 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
    following disclaimer in the documentation and/or other materials provided with the distribution.
 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
    products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include"t64_tundra.h"
#include"t64_xlat_4to6_icmp.h"

#include"t64_utils.h"
#include"t64_utils_ip.h"


static t64te_tundra__xlat_status _t64f_xlat_4to6_icmp__translate_echo_request_or_echo_reply_message(t64ts_tundra__xlat_thread_context *context);
static t64te_tundra__xlat_status _t64f_xlat_4to6_icmp__translate_destination_unreachable_message(t64ts_tundra__xlat_thread_context *context);
static t64te_tundra__xlat_status _t64f_xlat_4to6_icmp__translate_time_exceeded_message(t64ts_tundra__xlat_thread_context *context);
static t64te_tundra__xlat_status _t64f_xlat_4to6_icmp__translate_parameter_problem_message(t64ts_tundra__xlat_thread_context *context);
static t64te_tundra__xlat_status _t64f_xlat_4to6_icmp__translate_carried_ip_header_and_part_of_data(t64ts_tundra__xlat_thread_context *context);
static t64te_tundra__xlat_status _t64f_xlat_4to6_icmp__translate_carried_ipv4_address_to_ipv6_address(const t64ts_tundra__xlat_thread_context *context, const uint8_t *in_ipv4_address, uint8_t *out_ipv6_address);
static uint8_t _t64f_xlat_4to6_icmp__translate_parameter_problem_pointer_value(const uint8_t in_pointer);


t64te_tundra__xlat_status t64f_xlat_4to6_icmp__translate_icmpv4_to_icmpv6(t64ts_tundra__xlat_thread_context *context) {
    // When this function is called, 'in_packet' contains a valid IPv4 header and 'out_packet' contains a valid IPv6
    //  header translated from the header of 'in_packet'. 'out_packet->packet_size' is set to the size of the packet's
    //  header(s) and 'out_packet->payload_size' is zero. '*context->out_packet.ipv6_carried_protocol_field' is set to
    //  58 (ICMPv6).

    if(T64MM_UTILS_IP__IS_IPV4_PACKET_FRAGMENTED(context->in_packet.packet_ipv4hdr))
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    if(context->in_packet.payload_size < 8)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    if(context->configuration->translator_checksum_check_icmpv4 && t64f_utils_ip__calculate_rfc1071_checksum(&context->in_packet, false, false) != 0)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    // OUT-PACKET-REMAINING-BUFFER-SIZE: at least 1520 bytes - up to 48 bytes IPv6 header = at least 1472 bytes free; 8 bytes needed (for ICMPv6 header)

    switch(context->in_packet.payload_icmpv4hdr->type) {
        case 8: case 0: // Echo Request and Echo Reply
            if(_t64f_xlat_4to6_icmp__translate_echo_request_or_echo_reply_message(context) != T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION)
                return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;
            break;

        case 3: // Destination Unreachable
            if(_t64f_xlat_4to6_icmp__translate_destination_unreachable_message(context) != T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION)
                return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;
            break;

        case 11: // Time Exceeded
            if(_t64f_xlat_4to6_icmp__translate_time_exceeded_message(context) != T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION)
                return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;
            break;

        case 12: // Parameter Problem
            if(_t64f_xlat_4to6_icmp__translate_parameter_problem_message(context) != T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION)
                return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;
            break;

        default:
            // All other types, including:
            // - Information Request/Reply (Type 15 and Type 16)
            // - Timestamp and Timestamp Reply (Type 13 and Type 14)
            // - Address Mask Request/Reply (Type 17 and Type 18)
            // - ICMP Router Advertisement (Type 9)
            // - ICMP Router Solicitation (Type 10)
            // - Redirect (Type 5)
            // - Alternative Host Address (Type 6)
            // - Source Quench (Type 4)
            return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;
    }

    if(context->out_packet.payload_size < 8)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION; // Just to make sure...

    context->out_packet.payload_icmpv6hdr->icmp6_cksum = 0;
    context->out_packet.payload_icmpv6hdr->icmp6_cksum = t64f_utils_ip__calculate_rfc1071_checksum(&context->out_packet, true, false);

    return T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION;
}

// Types 8 and 0 - Echo Request and Echo Reply
static t64te_tundra__xlat_status _t64f_xlat_4to6_icmp__translate_echo_request_or_echo_reply_message(t64ts_tundra__xlat_thread_context *context) {
    // Copy the whole packet's payload
    if(!t64f_utils__secure_memcpy(
        context->out_packet.payload_raw,
        context->in_packet.payload_raw,
        context->in_packet.payload_size,
        (T64C_TUNDRA__MAX_PACKET_SIZE - context->out_packet.packet_size)
    )) return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    // Adjust type
    if(context->out_packet.payload_icmpv6hdr->icmp6_type == 8) // Echo request
        context->out_packet.payload_icmpv6hdr->icmp6_type = 128;
    else if(context->out_packet.payload_icmpv6hdr->icmp6_type == 0) // Echo reply
        context->out_packet.payload_icmpv6hdr->icmp6_type = 129;
    else
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION; // This should never happen!

    // Check code
    if(context->out_packet.payload_icmpv6hdr->icmp6_code != 0)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    context->out_packet.packet_size += context->in_packet.payload_size;
    context->out_packet.payload_size = context->in_packet.payload_size;

    return T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION;
}

// Type 3 - Destination Unreachable
static t64te_tundra__xlat_status _t64f_xlat_4to6_icmp__translate_destination_unreachable_message(t64ts_tundra__xlat_thread_context *context) {
    switch(context->in_packet.payload_icmpv4hdr->code) {
        case 0: case 1: case 5: case 6: case 7: case 8: case 11: case 12: // Net Unreachable, Host Unreachable, Source Route Failed
            {
                // Check inbound ICMPv4 header
                if(!T64M_UTILS__MEMORY_EQUAL(context->in_packet.payload_raw + 4, "\x00\x00\x00\x00", 4))
                    return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

                // Generate outbound ICMPv6 header
                t64f_utils_ip__generate_basic_icmpv4_or_icmpv6_header_to_empty_out_packet_payload(context, 1, 0);
            }
            break;

        case 9: case 10: case 13: case 15: // Communication with Destination Host Administratively Prohibited, Communication Administratively Prohibited, Precedence cutoff in effect
            {
                // Check inbound ICMPv4 header
                if(!T64M_UTILS__MEMORY_EQUAL(context->in_packet.payload_raw + 4, "\x00\x00\x00\x00", 4))
                    return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

                // Generate outbound ICMPv6 header
                t64f_utils_ip__generate_basic_icmpv4_or_icmpv6_header_to_empty_out_packet_payload(context, 1, 1);
            }
            break;

        case 3: // Port Unreachable
            {
                // Check inbound ICMPv4 header
                if(!T64M_UTILS__MEMORY_EQUAL(context->in_packet.payload_raw + 4, "\x00\x00\x00\x00", 4))
                    return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

                // Generate outbound ICMPv6 header
                t64f_utils_ip__generate_basic_icmpv4_or_icmpv6_header_to_empty_out_packet_payload(context, 1, 4);
            }
            break;

        case 4: // Fragmentation Needed and DF was Set
            {
                // Check inbound ICMPv4 header
                if(!T64M_UTILS__MEMORY_EQUAL(context->in_packet.payload_raw + 4, "\x00\x00", 2))
                    return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

                // Generate outbound ICMPv6 header
                t64f_utils_ip__generate_basic_icmpv4_or_icmpv6_header_to_empty_out_packet_payload(context, 2, 0);

                {
                    uint16_t mtu;
                    memcpy(&mtu, context->in_packet.payload_raw + 6, 2); // This is a hack which overcomes the 2-byte alignment requirement for 16-bit values.
                    mtu = ntohs(mtu);
                    if(mtu == 0)
                        mtu = (uint16_t) T64C_TUNDRA__RFC1191_IPV4_PLATEAU_MTU; // https://datatracker.ietf.org/doc/html/rfc1191#page-17
                    else if(mtu < 68 || mtu > 65515) // The minimum IPv6 MTU is 68 bytes, and the maximum valid MTU (in this case) is 65515, as a bigger MTU value would lead to integer overflow in the following calculations
                        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

                    // https://datatracker.ietf.org/doc/html/rfc7915#page-11
                    mtu = T64MM_UTILS__MINIMUM(mtu + 20, (uint16_t) context->configuration->translator_ipv6_outbound_mtu);
                    mtu = T64MM_UTILS__MINIMUM(mtu, ((uint16_t) context->configuration->translator_ipv4_outbound_mtu) + 20);
                    mtu = T64MM_UTILS__MAXIMUM(1280, mtu);

                    mtu = htons(mtu);
                    memcpy(context->out_packet.payload_raw + 6, &mtu, 2);
                }
            }
            break;

        case 2: // Protocol Unreachable
            {
                // Check inbound ICMPv4 header
                if(!T64M_UTILS__MEMORY_EQUAL(context->in_packet.payload_raw + 4, "\x00\x00\x00\x00", 4))
                    return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

                // Generate outbound ICMPv6 header
                t64f_utils_ip__generate_basic_icmpv4_or_icmpv6_header_to_empty_out_packet_payload(context, 4, 1);

                context->out_packet.payload_raw[7] = 6; // Pointer points to the "Next header" field
            }
            break;

        default:
            // All other codes, including:
            // - Code 14 (Host Precedence Violation)
            return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;
    }

    // Build carried IP header & part of data
    if(_t64f_xlat_4to6_icmp__translate_carried_ip_header_and_part_of_data(context) != T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    return T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION;
}

// Type 11 - Time Exceeded
static t64te_tundra__xlat_status _t64f_xlat_4to6_icmp__translate_time_exceeded_message(t64ts_tundra__xlat_thread_context *context) {
    // Check inbound ICMPv4 header
    if(context->in_packet.payload_icmpv4hdr->code != 0 && context->in_packet.payload_icmpv4hdr->code != 1)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    if(!T64M_UTILS__MEMORY_EQUAL(context->in_packet.payload_raw + 4, "\x00\x00\x00\x00", 4))
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    // Generate outbound ICMPv6 header
    t64f_utils_ip__generate_basic_icmpv4_or_icmpv6_header_to_empty_out_packet_payload(context, 3, context->in_packet.payload_icmpv4hdr->code);

    // Build carried IP header & part of data
    if(_t64f_xlat_4to6_icmp__translate_carried_ip_header_and_part_of_data(context) != T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    return T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION;
}

// Type 12 - Parameter Problem
static t64te_tundra__xlat_status _t64f_xlat_4to6_icmp__translate_parameter_problem_message(t64ts_tundra__xlat_thread_context *context) {
    // Check inbound ICMPv4 header
    if(context->in_packet.payload_icmpv4hdr->code != 0 && context->in_packet.payload_icmpv4hdr->code != 2)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    if(!T64M_UTILS__MEMORY_EQUAL(context->in_packet.payload_raw + 5, "\x00\x00\x00", 3))
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    // Generate outbound ICMPv6 header
    t64f_utils_ip__generate_basic_icmpv4_or_icmpv6_header_to_empty_out_packet_payload(context, 4, 0);

    {
        const uint8_t out_pointer = _t64f_xlat_4to6_icmp__translate_parameter_problem_pointer_value(context->in_packet.payload_raw[4]);
        if(out_pointer == 255)
            return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

        context->out_packet.payload_raw[7] = out_pointer;
    }

    // Build carried IP header & part of data
    if(_t64f_xlat_4to6_icmp__translate_carried_ip_header_and_part_of_data(context) != T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    return T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION;
}

static t64te_tundra__xlat_status _t64f_xlat_4to6_icmp__translate_carried_ip_header_and_part_of_data(t64ts_tundra__xlat_thread_context *context) {
    // Declaration & initialization of variables
    t64ts_tundra__packet in_ipv4_carried_packet;
    T64M_UTILS__MEMORY_CLEAR(&in_ipv4_carried_packet, 1, sizeof(t64ts_tundra__packet));
    in_ipv4_carried_packet.packet_raw = (context->in_packet.payload_raw + 8);
    in_ipv4_carried_packet.packet_size = (context->in_packet.payload_size - 8);

    t64ts_tundra__packet out_ipv6_carried_packet;
    T64M_UTILS__MEMORY_CLEAR(&out_ipv6_carried_packet, 1, sizeof(t64ts_tundra__packet));
    out_ipv6_carried_packet.packet_raw = (context->out_packet.payload_raw + 8);
    out_ipv6_carried_packet.packet_size = 40;

    // Size checks
    if(in_ipv4_carried_packet.packet_size < 20)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    // OUT-PACKET-REMAINING-BUFFER-SIZE: at least 1520 bytes - up to 48 bytes IPv6 header - 8 bytes ICMPv6 header = at least 1464 bytes free; up to 48 bytes needed (for IPv6 header + up to 8 bytes of payload)

    // IP header
    if(in_ipv4_carried_packet.packet_ipv4hdr->version != 4)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    const size_t in_ipv4_carried_packet_header_length = (in_ipv4_carried_packet.packet_ipv4hdr->ihl * 4);
    if(in_ipv4_carried_packet_header_length < 20 || in_ipv4_carried_packet_header_length > in_ipv4_carried_packet.packet_size)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    /*
     * https://datatracker.ietf.org/doc/html/rfc7915#page-14 states:
     * "The translation of the inner IP header can be done by invoking the
     *  function that translated the outer IP headers.  This process MUST
     *  stop at the first embedded header and drop the packet if it contains
     *  more embedded headers."
     */
    if(
        t64f_utils_ip__is_ip_protocol_number_forbidden(in_ipv4_carried_packet.packet_ipv4hdr->protocol) ||
        (in_ipv4_carried_packet.packet_ipv4hdr->protocol == 1) ||
        (in_ipv4_carried_packet.packet_ipv4hdr->protocol == 58)
    ) return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    if(T64MM_UTILS_IP__IS_IPV4_PACKET_FRAGMENTED(in_ipv4_carried_packet.packet_ipv4hdr))
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION; // Having a fragmented inbound IPv4 packet would mean that it is necessary to add a fragmentation header to the outbound IPv6 packet, which is, according to RFC 7915, forbidden

    out_ipv6_carried_packet.packet_ipv6hdr->version = 6;
    out_ipv6_carried_packet.packet_ipv6hdr->priority = (uint8_t) (in_ipv4_carried_packet.packet_ipv4hdr->tos >> 4);
    out_ipv6_carried_packet.packet_ipv6hdr->flow_lbl[0] = (uint8_t) (in_ipv4_carried_packet.packet_ipv4hdr->tos << 4);
    out_ipv6_carried_packet.packet_ipv6hdr->flow_lbl[1] = 0;
    out_ipv6_carried_packet.packet_ipv6hdr->flow_lbl[2] = 0;
    out_ipv6_carried_packet.packet_ipv6hdr->payload_len = htons(ntohs(in_ipv4_carried_packet.packet_ipv4hdr->tot_len) - ((uint16_t) in_ipv4_carried_packet_header_length));
    out_ipv6_carried_packet.packet_ipv6hdr->nexthdr = in_ipv4_carried_packet.packet_ipv4hdr->protocol;
    out_ipv6_carried_packet.packet_ipv6hdr->hop_limit = in_ipv4_carried_packet.packet_ipv4hdr->ttl;

    if(_t64f_xlat_4to6_icmp__translate_carried_ipv4_address_to_ipv6_address(context, (uint8_t *) &in_ipv4_carried_packet.packet_ipv4hdr->saddr, (uint8_t *) out_ipv6_carried_packet.packet_ipv6hdr->saddr.s6_addr) != T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;
    if(_t64f_xlat_4to6_icmp__translate_carried_ipv4_address_to_ipv6_address(context, (uint8_t *) &in_ipv4_carried_packet.packet_ipv4hdr->daddr, (uint8_t *) out_ipv6_carried_packet.packet_ipv6hdr->daddr.s6_addr) != T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    // Part of data
    size_t data_bytes_to_copy = (in_ipv4_carried_packet.packet_size - in_ipv4_carried_packet_header_length);
    if(data_bytes_to_copy > 8)
        data_bytes_to_copy = 8;

    memcpy(out_ipv6_carried_packet.packet_raw + 40, in_ipv4_carried_packet.packet_raw + in_ipv4_carried_packet_header_length, data_bytes_to_copy);
    out_ipv6_carried_packet.packet_size += data_bytes_to_copy;

    context->out_packet.packet_size += out_ipv6_carried_packet.packet_size;
    context->out_packet.payload_size += out_ipv6_carried_packet.packet_size;

    return T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION;
}

static t64te_tundra__xlat_status _t64f_xlat_4to6_icmp__translate_carried_ipv4_address_to_ipv6_address(const t64ts_tundra__xlat_thread_context *context, const uint8_t *in_ipv4_address, uint8_t *out_ipv6_address) {
    if(T64M_UTILS_IP__IPV4_ADDRESSES_EQUAL(in_ipv4_address, context->configuration->router_ipv4))
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION; // Packets from/to the router are not translated

    if(T64M_UTILS_IP__IPV4_ADDRESSES_EQUAL(in_ipv4_address, context->configuration->translator_ipv4)) {
        memcpy(out_ipv6_address, context->configuration->translator_ipv6, 16);
        return T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION;
    }

    // For the purposes of debugging, illegal addresses (such as 127.0.0.1) inside ICMP packets are translated normally.
    memcpy(out_ipv6_address, context->configuration->translator_prefix, 12);
    memcpy(out_ipv6_address + 12, in_ipv4_address, 4);
    return T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION;
}

// Returns 255 if 'in_pointer' is invalid and the translation process shall be stopped!
static uint8_t _t64f_xlat_4to6_icmp__translate_parameter_problem_pointer_value(const uint8_t in_pointer) {
    // https://datatracker.ietf.org/doc/html/rfc7915#page-13

    if(in_pointer == 0 || in_pointer == 1)
        return in_pointer;

    if(in_pointer == 2 || in_pointer == 3)
        return 4;

    if(in_pointer == 8)
        return 7;

    if(in_pointer == 9)
        return 6;

    if(in_pointer >= 12 && in_pointer <= 15)
        return 8;

    if(in_pointer >= 16 && in_pointer <= 19)
        return 24;

    return 255; // Includes the values 4, 5, 6, 7, 10, 11
}
