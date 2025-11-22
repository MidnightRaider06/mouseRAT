#include "../mouseRAT-client/FrameAssembler.h"

#include <cassert>
#include <string>

int main() {
    std::string payload(65536, '\0');
    payload[1] = static_cast<char>(0xff);
    payload.replace(32768, 4, "MRAT");
    const auto image = protocol::encodeHeader(0x22, 65536, 7);
    const auto ack = protocol::encodeHeader(0x02, 0, 0);
    std::string wire(reinterpret_cast<const char*>(image.data()), image.size());
    wire += payload;
    wire.append(reinterpret_cast<const char*>(ack.data()), ack.size());

    for (const std::size_t chunk : { 1u, 2u, 15u, 16u, 17u, 4096u, 65552u, 65568u }) {
        protocol::FrameAssembler assembler;
        unsigned delivered = 0;
        std::size_t offset = 0;
        while (offset < wire.size()) {
            std::string_view input(wire.data() + offset, (std::min)(chunk, wire.size() - offset));
            while (!input.empty()) {
                std::size_t consumed;
                const auto frame = assembler.consume(input, consumed);
                assert(consumed != 0 && consumed <= input.size());
                input.remove_prefix(consumed);
                offset += consumed;
                if (frame) {
                    assert(delivered < 2);
                    assert(frame->header.type == (delivered == 0 ? 0x22 : 0x02));
                    assert(frame->header.requestId == (delivered == 0 ? 7u : 0u));
                    assert(frame->payload == (delivered == 0 ? std::string_view(payload) : ""));
                    ++delivered;
                }
            }
        }

        std::size_t consumed;
        assert(delivered == 2 && !assembler.hasPartialFrame());
        assert(!assembler.consume({}, consumed) && consumed == 0);
    }
}
