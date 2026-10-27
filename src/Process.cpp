#include <Process.hpp>
#include <Wire.hpp>
#include <cstdint>
#include <algorithm>

Process::Process(ID id) : identity(id) {}

void Process::start(GraphFrame frame) {
  if (frame.kind != FrameKind::Data) {
    throw std::invalid_argument("A wave must be started with a data frame");
  }
  if (parent || activeWaves.contains(frame.waveID) || completedWaves.contains(frame.waveID)) return;
  activeWaves.insert(frame.waveID);
  std::println("Process Tree ROOT has initiated a message group.");
  go(frame);
}

void Process::go(GraphFrame frame) {
  std::vector<Channel*> childChannels;
  childChannels.reserve(children.size());
  for (ID childID : children) {
    auto it = activeChannels.find(childID);
    if (it == activeChannels.end()) {
      throw std::runtime_error("No channel registered for child process " + std::to_string(childID));
    }
    childChannels.push_back(&it->second);
  }

  expectedMessagesByWave.insert_or_assign(frame.waveID, childChannels.size());
  if (childChannels.empty()) {
    completeWave(frame.waveID);
    return;
  }

  for (Channel* channel : childChannels) {
    std::visit([&frame](auto& implementation) { implementation.send(frame); }, *channel);
  }
}

void Process::back(GraphFrame backFrame) {
  if (backFrame.kind != FrameKind::Acknowledgement) return;

  auto outstanding = expectedMessagesByWave.find(backFrame.waveID);
  if (outstanding == expectedMessagesByWave.end() || outstanding->second == 0) return;
  if (--outstanding->second == 0) {
    completeWave(backFrame.waveID);
  }
}

void Process::completeWave(WaveID waveID) {
  expectedMessagesByWave.erase(waveID);
  activeWaves.erase(waveID);
  completedWaves.insert(waveID);

  if (parent) {
    auto it = activeChannels.find(*parent);
    if (it == activeChannels.end()) {
      throw std::runtime_error("No channel registered for parent process " + std::to_string(*parent));
    }
    GraphFrame acknowledgement{waveID, {}, FrameKind::Acknowledgement};
    std::visit(
        [&acknowledgement](auto& channel) { channel.send(acknowledgement); },
        it->second
    );
    return;
  }

  std::println("Process Tree ROOT has completed Wave {}. Initiating global cleanup broadcast.", waveID);
  GraphFrame cleanup{waveID, {}, FrameKind::Cleanup};
  for (ID childID : children) {
    auto it = activeChannels.find(childID);
    if (it == activeChannels.end()) {
      throw std::runtime_error("No channel registered for child process " + std::to_string(childID));
    }
    std::visit([&cleanup](auto& channel) { channel.send(cleanup); }, it->second);
  }
}

void Process::pollChannels() {
  for (auto& [neighbourID, channel] : activeChannels) {
    (void)neighbourID;
    if (auto* socket = std::get_if<SocketChannel>(&channel)) {
      socket->receive(*this);
    }
  }
}

bool Process::isWaveCompleted(WaveID waveID) const {
  return completedWaves.contains(waveID);
}

bool Process::isWaveCleaned(WaveID waveID) const {
  return cleanedWaves.contains(waveID);
}

void Process::position(ID id, std::vector<Edge> neighbours) {
  // FIX: Swapped 'identity' out for the correct input variable 'id'
  if (std::find(knownProcesses.begin(), knownProcesses.end(), id) == knownProcesses.end()) {
    knownProcesses.push_back(id);
  }

  for (auto &edge : neighbours) {
    if (std::find(knownChannels.begin(), knownChannels.end(), edge) == knownChannels.end()) {
      knownChannels.push_back(edge);
    }
  }
}

void Process::receive(GraphFrame frame) {
    if (frame.kind == FrameKind::Cleanup) {
        if (!cleanedWaves.insert(frame.waveID).second) return;
        completedWaves.insert(frame.waveID);
        activeWaves.erase(frame.waveID);
        expectedMessagesByWave.erase(frame.waveID);

        for (ID childID : children) {
            auto it = activeChannels.find(childID);
            if (it == activeChannels.end()) {
                throw std::runtime_error("No channel registered for child process " + std::to_string(childID));
            }
            std::visit([&frame](auto& channel) { channel.send(frame); }, it->second);
        }
        return;
    }

    if (completedWaves.contains(frame.waveID)) return; 

    if (frame.kind == FrameKind::Acknowledgement) {
        back(frame);
        return;
    }

    if (!activeWaves.insert(frame.waveID).second) return;
    go(frame);
}

void Process::registerChannel(ID neighbourID, Channel channelImplementation) {
  activeChannels.insert_or_assign(neighbourID, std::move(channelImplementation));
}

void Process::addChild(Process *child) {
  if (!child) throw std::invalid_argument("Child process cannot be null");
  if (child->parent && child->parent != identity) {
    throw std::invalid_argument("Child process already has a different parent");
  }
  addChild(child->identity);
  child->parent = identity;
}

void Process::addChild(ID childID) {
  if (childID == identity) throw std::invalid_argument("A process cannot be its own child");
  if (std::find(children.begin(), children.end(), childID) == children.end()) {
    children.push_back(childID);
  }
}

void Process::setParent(ID parentID) {
  if (parentID == identity) throw std::invalid_argument("A process cannot be its own parent");
  if (parent && parent != parentID) {
    throw std::invalid_argument("Process already has a different parent");
  }
  parent = parentID;
}
