#include "CentralCoordinatorIDGen.hpp"
#include "Process.hpp"
#include "Wire.hpp"

#include <array>
#include <cerrno>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template<typename Exception, typename Callable>
void requireThrows(Callable&& callable, const std::string& message) {
    try {
        std::forward<Callable>(callable)();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

std::array<int, 2> makeSocketPair() {
    std::array<int, 2> sockets{};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets.data()) < 0) {
        throw std::system_error(errno, std::generic_category(), "socketpair");
    }
    return sockets;
}

void testIdGeneration() {
    CentralCoordinatorIDGen generator(3);
    const uint16_t first = generator.generate_id();
    const uint16_t second = generator.generate_id();

    require((first >> 12) == 3, "Generated ID does not contain the configured server ID");
    require((second >> 12) == 3, "Second generated ID does not contain the configured server ID");
    require(first != second, "Consecutive generated IDs are not unique");
    require((second & 0x0FFF) == ((first + 1) & 0x0FFF),
            "Generated sequence did not advance by one");
    requireThrows<std::invalid_argument>(
        [] { CentralCoordinatorIDGen invalidGenerator(16); },
        "Generator accepted a server ID outside the 4-bit range"
    );
}

void testTopologyValidation() {
    Process parent(10);
    Process child(11);
    Process otherParent(12);

    requireThrows<std::invalid_argument>(
        [&] { parent.addChild(&parent); },
        "A process accepted itself as a child"
    );
    parent.addChild(&child);
    requireThrows<std::invalid_argument>(
        [&] { otherParent.addChild(&child); },
        "A child accepted a second, conflicting parent"
    );
    requireThrows<std::invalid_argument>(
        [&] { child.setParent(child.identity); },
        "A process accepted itself as its parent"
    );
}

void testLocalTreeLifecycle() {
    Process root(100);
    Process branch(200);
    Process leftLeaf(300);
    Process rightLeaf(400);

    root.addChild(&branch);
    root.addChild(&rightLeaf);
    branch.addChild(&leftLeaf);

    root.registerChannel(branch.identity, PointerChannel<Process>(&branch));
    root.registerChannel(rightLeaf.identity, PointerChannel<Process>(&rightLeaf));
    branch.registerChannel(root.identity, PointerChannel<Process>(&root));
    branch.registerChannel(leftLeaf.identity, PointerChannel<Process>(&leftLeaf));
    leftLeaf.registerChannel(branch.identity, PointerChannel<Process>(&branch));
    rightLeaf.registerChannel(root.identity, PointerChannel<Process>(&root));

    const auto requireCompletedAndCleaned = [&](WaveID waveID) {
        require(root.isWaveCompleted(waveID), "Root did not complete wave " + std::to_string(waveID));
        require(branch.isWaveCompleted(waveID), "Branch did not complete wave " + std::to_string(waveID));
        require(leftLeaf.isWaveCompleted(waveID), "Left leaf did not complete wave " + std::to_string(waveID));
        require(rightLeaf.isWaveCompleted(waveID), "Right leaf did not complete wave " + std::to_string(waveID));
        require(branch.isWaveCleaned(waveID), "Branch did not receive cleanup for wave " + std::to_string(waveID));
        require(leftLeaf.isWaveCleaned(waveID), "Left leaf did not receive cleanup for wave " + std::to_string(waveID));
        require(rightLeaf.isWaveCleaned(waveID), "Right leaf did not receive cleanup for wave " + std::to_string(waveID));
    };

    // Control traffic must not depend on the application payload's contents.
    const GraphFrame textThatLooksLikeControl{501, {'C', 'L', 'E', 'A', 'N', 'U', 'P'}, FrameKind::Data};
    root.start(textThatLooksLikeControl);
    requireCompletedAndCleaned(textThatLooksLikeControl.waveID);

    // An empty application payload is still data because its frame kind is explicit.
    const GraphFrame emptyData{502, {}, FrameKind::Data};
    root.start(emptyData);
    requireCompletedAndCleaned(emptyData.waveID);

    // Exercise several independent waves and ensure completed waves cannot restart.
    const GraphFrame anotherWave{503, {0, 1, 2, 255}, FrameKind::Data};
    root.start(anotherWave);
    requireCompletedAndCleaned(anotherWave.waveID);
    root.start(anotherWave);
    requireCompletedAndCleaned(anotherWave.waveID);

    // Replayed data, acknowledgements, and cleanup must not change completed state.
    branch.receive(textThatLooksLikeControl);
    root.receive(GraphFrame{501, {}, FrameKind::Acknowledgement});
    branch.receive(GraphFrame{501, {}, FrameKind::Cleanup});
    requireCompletedAndCleaned(501);

    // A non-root process cannot initiate a root wave.
    leftLeaf.start(GraphFrame{504, {'x'}, FrameKind::Data});
    require(!leftLeaf.isWaveCompleted(504), "A child process incorrectly started a root wave");

    requireThrows<std::invalid_argument>(
        [&] { root.start(GraphFrame{505, {}, FrameKind::Acknowledgement}); },
        "Root accepted a non-data frame as a new wave"
    );

    Process singleton(500);
    singleton.start(GraphFrame{506, {}, FrameKind::Data});
    require(singleton.isWaveCompleted(506), "A root with no children did not complete its wave");
}

class FrameCollector {
public:
    std::vector<GraphFrame> frames;

    void receive(GraphFrame frame) {
        frames.push_back(std::move(frame));
    }
};

void testWireFrameRoundTrip() {
    const auto sockets = makeSocketPair();
    auto sender = std::make_shared<Wire>(sockets[0]);
    Wire receiver(sockets[1]);
    FrameCollector collector;

    const std::vector<GraphFrame> expected{
        {601, {}, FrameKind::Data},
        {602, {'C', 'L', 'E', 'A', 'N', 'U', 'P'}, FrameKind::Data},
        {603, {0, 255, 17}, FrameKind::Acknowledgement},
        {604, {}, FrameKind::Cleanup}
    };

    for (const GraphFrame& frame : expected) {
        require(sender->sendFrame(frame), "Wire failed to send a frame");
    }
    require(receiver.receiveAndParse(collector), "Wire did not report receiving frames");
    require(collector.frames.size() == expected.size(),
            "Wire did not parse every frame from the socket stream");

    for (size_t i = 0; i < expected.size(); ++i) {
        require(collector.frames[i].waveID == expected[i].waveID,
                "Wire changed a frame's wave ID");
        require(collector.frames[i].kind == expected[i].kind,
                "Wire changed a frame's kind");
        require(collector.frames[i].payload == expected[i].payload,
                "Wire changed a frame's payload");
    }
}

void testSocketBackedGraphLifecycle() {
    Process root(700);
    Process firstChild(701);
    Process secondChild(702);

    root.addChild(firstChild.identity);
    root.addChild(secondChild.identity);
    firstChild.setParent(root.identity);
    secondChild.setParent(root.identity);

    const auto firstSockets = makeSocketPair();
    const auto secondSockets = makeSocketPair();

    root.registerChannel(
        firstChild.identity,
        SocketChannel(std::make_shared<Wire>(firstSockets[0]))
    );
    firstChild.registerChannel(
        root.identity,
        SocketChannel(std::make_shared<Wire>(firstSockets[1]))
    );
    root.registerChannel(
        secondChild.identity,
        SocketChannel(std::make_shared<Wire>(secondSockets[0]))
    );
    secondChild.registerChannel(
        root.identity,
        SocketChannel(std::make_shared<Wire>(secondSockets[1]))
    );

    constexpr WaveID waveID = 701;
    const GraphFrame frame{waveID, {'s', 'o', 'c', 'k', 'e', 't'}, FrameKind::Data};
    root.start(frame);
    root.start(frame);

    firstChild.pollChannels();
    root.pollChannels();
    require(firstChild.isWaveCompleted(waveID), "First socket child did not complete its wave");
    require(!root.isWaveCompleted(waveID),
            "Root completed before receiving acknowledgements from every child");

    secondChild.pollChannels();
    root.pollChannels();
    require(secondChild.isWaveCompleted(waveID), "Second socket child did not complete its wave");
    require(root.isWaveCompleted(waveID), "Root did not complete after all child acknowledgements");

    firstChild.pollChannels();
    secondChild.pollChannels();
    require(firstChild.isWaveCleaned(waveID), "First socket child did not receive cleanup");
    require(secondChild.isWaveCleaned(waveID), "Second socket child did not receive cleanup");
}

} // namespace

int main() {
    const std::vector<std::pair<std::string, std::function<void()>>> tests{
        {"ID generation and validation", testIdGeneration},
        {"Topology validation", testTopologyValidation},
        {"Local branching tree lifecycle", testLocalTreeLifecycle},
        {"Wire frame round-trip", testWireFrameRoundTrip},
        {"Socket-backed graph lifecycle", testSocketBackedGraphLifecycle}
    };

    size_t failures = 0;
    std::println("Running {} graph integration tests...", tests.size());

    for (const auto& [name, test] : tests) {
        try {
            test();
            std::println("[PASS] {}", name);
        } catch (const std::exception& error) {
            ++failures;
            std::println("[FAIL] {}: {}", name, error.what());
        }
    }

    std::println(
        "\n{} tests passed; {} failed.",
        tests.size() - failures,
        failures
    );
    return failures == 0 ? 0 : 1;
}
