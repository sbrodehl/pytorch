#include <torch/csrc/distributed/rpc/message.h>

namespace torch {
namespace distributed {
namespace rpc {

Message::Message() = default;

Message::Message(
    std::vector<char>&& payload,
    std::vector<torch::Tensor>&& tensors,
    MessageType type)
    : payload_(std::move(payload)), tensors_(std::move(tensors)), type_(type) {}

Message::Message(
    std::vector<char>&& payload,
    std::vector<torch::Tensor>&& tensors,
    MessageType type,
    int64_t id)
    : payload_(std::move(payload)),
      tensors_(std::move(tensors)),
      type_(type),
      id_(id) {}

std::vector<char>&& Message::movePayload() && {
  return std::move(payload_);
}

std::vector<char>& Message::payload() {
  return payload_;
}

const std::vector<char>& Message::payload() const {
  return payload_;
}

std::vector<torch::Tensor>&& Message::moveTensors() && {
  return std::move(tensors_);
}

std::vector<torch::Tensor>& Message::tensors() {
  return tensors_;
}

const std::vector<torch::Tensor>& Message::tensors() const {
  return tensors_;
}

MessageType Message::type() const {
  return type_;
}

bool Message::isRequest() const {
  return MessageTypeFlags::REQUEST_TYPE & type_;
}

bool Message::isResponse() const {
  return MessageTypeFlags::RESPONSE_TYPE & type_;
}

int64_t Message::id() const {
  return id_;
}

void Message::setId(int64_t id) {
  id_ = id;
}

std::vector<std::reference_wrapper<const at::DataPtr>> Message::getDataPtrs()
    const {
  std::vector<std::reference_wrapper<const at::DataPtr>> dataPtrs;
  dataPtrs.reserve(tensors_.size());
  for (const auto& tensor : tensors_) {
    dataPtrs.emplace_back(tensor.storage().data_ptr());
  }
  return dataPtrs;
}

c10::intrusive_ptr<Message> createExceptionResponse(
    const std::exception& e,
    int64_t id) {
  return createExceptionResponse(e.what(), id);
}

c10::intrusive_ptr<Message> createExceptionResponse(
    const std::string& exceptionStr,
    int64_t id) {
  std::vector<char> payload(exceptionStr.begin(), exceptionStr.end());
  return c10::make_intrusive<Message>(
      std::move(payload),
      std::vector<torch::Tensor>(),
      MessageType::EXCEPTION,
      id);
}

namespace {

// NB: need to call torch::class_ to register Message in the map returned by
// c10::getCustomClassTypeMap(). Otherwise, Message cannot be wrapped within
// an IValue.
// NB: add this line here instead of in rpc/init.cpp because 1) we have C++
// only tests that won't run rpc/init.cpp; 2) Message is not meant to be
// visible from Python.
static const auto message = torch::class_<Message>("rpc", "_Message");

} // namespace

} // namespace rpc
} // namespace distributed
} // namespace torch
