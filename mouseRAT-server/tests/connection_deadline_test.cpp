#include "Connection.h"

#include <cassert>

using namespace std::chrono_literals;

int main() {
    Connection client;
    const Connection::Clock::time_point accepted{};
    client.acceptedAt = accepted;
    client.lastFrameAt = accepted;
    assert(client.nextDeadline() == accepted + 5s);

    client.frameStarted = accepted + 1s;
    assert(client.nextDeadline() == accepted + 5s);
    client.helloReceived = true;
    client.frameStarted.reset();
    assert(!client.nextDeadline());
    assert(client.nextWakeup() == accepted + 10s);

    client.frameStarted = accepted + 2s;
    assert(client.nextDeadline() == accepted + 32s);
    client.frameStarted.reset();
    assert(!client.nextDeadline());
    client.frameStarted = accepted + 20s;
    assert(client.nextDeadline() == accepted + 50s);

    client.requestDeadline = accepted + 300s;
    assert(client.nextDeadline() == accepted + 50s);
    client.frameStarted = accepted + 290s;
    assert(client.nextDeadline() == accepted + 300s);
    client.frameStarted.reset();
    assert(client.nextDeadline() == accepted + 300s);
    client.requestDeadline.reset();
    assert(!client.nextDeadline());

    client.pongDeadline = accepted + 40s;
    assert(client.nextDeadline() == accepted + 40s);
    assert(client.nextWakeup() == accepted + 40s);
    client.lastFrameAt = accepted + 25s;
    assert(client.nextDeadline() == accepted + 40s);
    client.frameStarted = accepted + 1s;
    assert(client.nextDeadline() == accepted + 31s);
    client.frameStarted.reset();
    client.pongDeadline.reset();
    assert(!client.nextDeadline());
    assert(client.nextWakeup() == accepted + 35s);
}
