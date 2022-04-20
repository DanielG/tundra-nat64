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
#include"t64_xlat_4to6.h"

#include"t64_utils.h"
#include"t64_utils_ip.h"
#include"t64_xlat.h"
#include"t64_xlat_4to6_icmp.h"
#include"t64_router_ipv4.h"


static t64te_tundra__xlat_status _t64f_xlat_4to6__evaluate_in_packet(t64ts_tundra__xlat_thread_context *context);
static t64te_tundra__xlat_status _t64f_xlat_4to6__translate_in_packet_headers_to_out_packet_headers(t64ts_tundra__xlat_thread_context *context);
static t64te_tundra__xlat_status _t64f_xlat_4to6__translate_in_packet_payload_to_out_packet_payload(t64ts_tundra__xlat_thread_context *context);


void t64f_xlat_4to6__handle_packet(t64ts_tundra__xlat_thread_context *context) {
    // 'context->in_packet.packet_raw' contains an unvalidated IPv4 (version = 4) packet and 'context->in_packet.packet_size' contains its size.
    // All the other characteristics of the packet, such as 'context->in_packet.payload_raw' and 'context->in_packet.payload_size', are undefined at this point.
    if(_t64f_xlat_4to6__evaluate_in_packet(context) != T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION)
        return;

    // At this moment, the entire in_packet's IPv4 header has been validated (including any IPv4 options);
    //  therefore, it is now safe to send ICMP messages back to the packet's origin host.
    if(_t64f_xlat_4to6__translate_in_packet_headers_to_out_packet_headers(context) != T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION)
        return;

    if(_t64f_xlat_4to6__translate_in_packet_payload_to_out_packet_payload(context) != T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION)
        return;

    // The IPv6 'out_packet' is now finished - it can be sent out.
    if(T64M_UTILS_IP__GET_IPV4_DONT_FRAGMENT_BIT(context->in_packet.packet_ipv4hdr)) {
        if(T64M_UTILS_IP__IPV6_PACKET_NEEDS_FRAGMENTATION(context, &context->out_packet))
            // Why (IPv6 MTU - 28)? "Worst case scenario" example: The IPv6 MTU is 1280 bytes; the IPv4 host sends a
            //  1252-byte (1280 - 28) fragmented IPv4 packet whose header has 20 bytes; during translation, the IPv4
            //  header is stripped, resulting in 1232 bytes of data; the 40-byte IPv6 header and 8-byte fragmentation
            //  extension header is prepended to the data, resulting in a 1280-byte IPv6 packet (the biggest packet that
            //  fits into the IPv6 MTU)
            t64f_router_ipv4__generate_and_send_icmpv4_fragmentation_needed_message_back_to_in_ipv4_packet_source_host(context, ((uint16_t) context->configuration->translator_ipv6_outbound_mtu) - 28);
        else
            t64f_xlat__finalize_and_send_specified_ipv6_packet(context, &context->out_packet);
    } else {
        t64f_xlat__possibly_fragment_and_send_ipv6_out_packet(context);
    }
}

static t64te_tundra__xlat_status _t64f_xlat_4to6__evaluate_in_packet(t64ts_tundra__xlat_thread_context *context) {
    if(context->in_packet.packet_size < 20)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION; // The smallest possible IPv4 header is 20 bytes in size.

    // Version is guaranteed to be 4

    // IHL
    const size_t header_length = (context->in_packet.packet_ipv4hdr->ihl * 4); // in bytes
    if(header_length < 20 || header_length > context->in_packet.packet_size)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    // DSCP and ECN need not be checked

    // Total length
    if(ntohs(context->in_packet.packet_ipv4hdr->tot_len) != context->in_packet.packet_size)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    // Identification, DF bit, MF bit and fragment offset need not be checked

    // Reserved bit (next to the DF & MF bits)
    if(T64M_UTILS_IP__GET_IPV4_FRAGMENT_RESERVED_BIT(context->in_packet.packet_ipv4hdr) != 0)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION; // The reserved bit must be zero

    // TTL
    if(context->in_packet.packet_ipv4hdr->ttl < 1)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION; // The packet should have already been dropped!

    // Protocol
    if(
        t64f_utils_ip__is_ip_protocol_number_forbidden(context->in_packet.packet_ipv4hdr->protocol) ||
        (context->in_packet.packet_ipv4hdr->protocol == 58) // ICMP for IPv6
    ) return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    // Source address
    if(!t64f_utils_ip__is_ipv4_embedded_ipv6_address_translatable(context, (uint8_t *) &context->in_packet.packet_ipv4hdr->saddr))
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    // Destination address
    if(!T64M_UTILS_IP__IPV4_ADDRESSES_EQUAL(&context->in_packet.packet_ipv4hdr->daddr, context->configuration->translator_ipv4))
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    // IPv4 Options
    {
        /*
         * From RFC 7915, section 4.1:
         *  If any IPv4 options are present in the IPv4 packet, they MUST be
         *  ignored and the packet translated normally; there is no attempt to
         *  translate the options.  However, if an unexpired source route option
         *  is present, then the packet MUST instead be discarded, and an ICMPv4
         *  "Destination Unreachable, Source Route Failed" (Type 3, Code 5) error
         *  message SHOULD be returned to the sender.
         */
        uint8_t *current_option_ptr = (context->in_packet.packet_raw + 20);
        ssize_t remaining_options_size = (((ssize_t) header_length) - 20);

        while(remaining_options_size > 0) {
            const uint8_t option_type = *current_option_ptr;
            if(option_type == 131 || option_type == 137) // Loose Source Route, Strict Source Route
                return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

            size_t current_option_size; // https://www.iana.org/assignments/ip-parameters/ip-parameters.xhtml
            if(option_type == 0 || option_type == 1) { // End of Options List, No Operation
                current_option_size = 1;
            } else {
                if(remaining_options_size < 2)
                    return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

                current_option_size = current_option_ptr[1];
                if(current_option_size < 2)
                    return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;
            }

            current_option_ptr += current_option_size;
            remaining_options_size -= current_option_size;
            if(remaining_options_size < 0)
                return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;
        }
    }

    // Header checksum
    if(context->configuration->translator_checksum_check_ipv4_header && t64f_utils_ip__calculate_ipv4_header_checksum(context->in_packet.packet_ipv4hdr) != 0)
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    context->in_packet.payload_raw = (context->in_packet.packet_raw + header_length);
    context->in_packet.payload_size = (context->in_packet.packet_size - header_length);

    return T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION;
}

static t64te_tundra__xlat_status _t64f_xlat_4to6__translate_in_packet_headers_to_out_packet_headers(t64ts_tundra__xlat_thread_context *context) {
    // OUT-PACKET-REMAINING-BUFFER-SIZE: at least 1520 bytes free; up to 48 bytes needed (for IPv6 header and optionally IPv6 fragmentation extension header)

    // Version
    context->out_packet.packet_ipv6hdr->version = 6;

    // Traffic class & Flow label
    if(context->configuration->translator_4to6_copy_dscp_and_ecn) {
        context->out_packet.packet_ipv6hdr->priority = (uint8_t) (context->in_packet.packet_ipv4hdr->tos >> 4);
        context->out_packet.packet_ipv6hdr->flow_lbl[0] = (uint8_t) (context->in_packet.packet_ipv4hdr->tos << 4);
    } else {
        context->out_packet.packet_ipv6hdr->priority = 0;
        context->out_packet.packet_ipv6hdr->flow_lbl[0] = 0;
    }
    context->out_packet.packet_ipv6hdr->flow_lbl[1] = 0;
    context->out_packet.packet_ipv6hdr->flow_lbl[2] = 0;

    // Payload length
    context->out_packet.packet_ipv6hdr->payload_len = 0; // This is set to a correct value when the packet is sent (at this moment, it is not known what the final size of the packet's payload will be)

    // Hop limit
    context->out_packet.packet_ipv6hdr->hop_limit = (context->in_packet.packet_ipv4hdr->ttl - 1);
    if(context->out_packet.packet_ipv6hdr->hop_limit < 1) {
        t64f_router_ipv4__generate_and_send_icmpv4_time_exceeded_message_back_to_in_ipv4_packet_source_host(context);
        return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;
    }

    // Source address
    memcpy((uint8_t *) context->out_packet.packet_ipv6hdr->saddr.s6_addr, context->configuration->translator_prefix, 12);
    memcpy(((uint8_t *) context->out_packet.packet_ipv6hdr->saddr.s6_addr) + 12, &context->in_packet.packet_ipv4hdr->saddr, 4);

    // Destination address
    memcpy((uint8_t *) context->out_packet.packet_ipv6hdr->daddr.s6_addr, context->configuration->translator_ipv6, 16);

    // Next header & Fragmentation
    if(T64MM_UTILS_IP__IS_IPV4_PACKET_FRAGMENTED(context->in_packet.packet_ipv4hdr)) {
        context->out_packet.packet_ipv6hdr->nexthdr = 44;

        // Fragment extension header setup
        t64ts_tundra__ipv6_fragment_header *ipv6_fragment_header = (t64ts_tundra__ipv6_fragment_header *) (context->out_packet.packet_raw + 40);
        ipv6_fragment_header->next_header = context->in_packet.packet_ipv4hdr->protocol;
        ipv6_fragment_header->reserved = 0;
        ipv6_fragment_header->offset_and_flags = T64M_UTILS_IP__CONSTRUCT_IPV6_FRAGMENT_OFFSET_AND_FLAGS_FIELD(
            T64M_UTILS_IP__GET_IPV4_FRAGMENT_OFFSET(context->in_packet.packet_ipv4hdr),
            T64M_UTILS_IP__GET_IPV4_MORE_FRAGMENTS_BIT(context->in_packet.packet_ipv4hdr)
        );
        ipv6_fragment_header->identification[0] = 0;
        ipv6_fragment_header->identification[1] = context->in_packet.packet_ipv4hdr->id;

        context->out_packet.packet_size = 48;
        context->out_packet.payload_raw = (context->out_packet.packet_raw + 48);
        context->out_packet.ipv6_fragment_header = ipv6_fragment_header;
        context->out_packet.ipv6_carried_protocol_field = &ipv6_fragment_header->next_header;
    } else {
        context->out_packet.packet_ipv6hdr->nexthdr = context->in_packet.packet_ipv4hdr->protocol;

        context->out_packet.packet_size = 40;
        context->out_packet.payload_raw = (context->out_packet.packet_raw + 40);
        context->out_packet.ipv6_fragment_header = NULL;
        context->out_packet.ipv6_carried_protocol_field = &context->out_packet.packet_ipv6hdr->nexthdr;
    }
    context->out_packet.payload_size = 0;

    return T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION;
}

static t64te_tundra__xlat_status _t64f_xlat_4to6__translate_in_packet_payload_to_out_packet_payload(t64ts_tundra__xlat_thread_context *context) {
    // Translation from ICMPv4 to ICMPv6 requires a special workflow
    if(*context->out_packet.ipv6_carried_protocol_field == 1) {
        *context->out_packet.ipv6_carried_protocol_field = 58;
        return t64f_xlat_4to6_icmp__translate_icmpv4_to_icmpv6(context);
    }

    // Data of other transport protocols are simply copied from 'in_packet' to 'out_packet'
    if(!t64f_utils__secure_memcpy(
        context->out_packet.payload_raw,
        context->in_packet.payload_raw,
        context->in_packet.payload_size,
        (T64C_TUNDRA__MAX_PACKET_SIZE - context->out_packet.packet_size)
    )) return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

    context->out_packet.packet_size += context->in_packet.payload_size;
    context->out_packet.payload_size = context->in_packet.payload_size;

    // However, some transport protocols contain checksums whose correct value changes when performing NAT64 translation
    if(!T64MM_UTILS_IP__IS_IPV4_PACKET_FRAGMENTED(context->in_packet.packet_ipv4hdr)) {
        if(*context->out_packet.ipv6_carried_protocol_field == 6 && context->out_packet.payload_size >= 20) { // TCP
            if(context->configuration->translator_checksum_check_tcp && t64f_utils_ip__calculate_rfc1071_checksum(&context->in_packet, true, false) != 0)
                return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

            context->out_packet.payload_tcphdr->check = 0;
            context->out_packet.payload_tcphdr->check = t64f_utils_ip__calculate_rfc1071_checksum(&context->out_packet, true, false);
        } else if(*context->out_packet.ipv6_carried_protocol_field == 17 && context->out_packet.payload_size >= 8) { // UDP
            if(context->configuration->translator_checksum_check_udp && context->in_packet.payload_size >= 8 && context->in_packet.payload_udphdr->check != 0 && t64f_utils_ip__calculate_rfc1071_checksum(&context->in_packet, true, false) != 0)
                return T64TE_TUNDRA__XLAT_STATUS_STOP_TRANSLATION;

            context->out_packet.payload_udphdr->check = 0;
            context->out_packet.payload_udphdr->check = t64f_utils_ip__calculate_rfc1071_checksum(&context->out_packet, true, true);
        }
    }

    return T64TE_TUNDRA__XLAT_STATUS_CONTINUE_TRANSLATION;
}
