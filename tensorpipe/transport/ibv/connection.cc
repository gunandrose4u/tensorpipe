/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <tensorpipe/transport/ibv/connection.h>

#include <string.h>

#include <deque>
#include <vector>

#include <tensorpipe/common/callback.h>
#include <tensorpipe/common/defs.h>
#include <tensorpipe/common/epoll_loop.h>
#include <tensorpipe/common/error_macros.h>
#include <tensorpipe/common/ibv.h>
#include <tensorpipe/common/memory.h>
#include <tensorpipe/common/ringbuffer_read_write_ops.h>
#include <tensorpipe/common/socket.h>
#include <tensorpipe/transport/error.h>
#include <tensorpipe/transport/ibv/context_impl.h>
#include <tensorpipe/transport/ibv/error.h>
#include <tensorpipe/transport/ibv/reactor.h>
#include <tensorpipe/transport/ibv/sockaddr.h>
#include <tensorpipe/util/ringbuffer/consumer.h>
#include <tensorpipe/util/ringbuffer/producer.h>

namespace tensorpipe {
namespace transport {
namespace ibv {

namespace {

constexpr auto kBufferSize = 2 * 1024 * 1024;

// When the connection gets closed, to avoid leaks, it needs to "reclaim" all
// the work requests that it had posted, by waiting for their completion. They
// may however complete with error, which makes it harder to identify and
// distinguish them from failing incoming requests because, in principle, we
// cannot access the opcode field of a failed work completion. Therefore, we
// assign a special ID to those types of requests, to match them later on.
constexpr uint64_t kWriteRequestId = 1;
constexpr uint64_t kAckRequestId = 2;

// The data that each queue pair endpoint needs to send to the other endpoint in
// order to set up the queue pair itself. This data is transferred over a TCP
// connection.
struct Exchange {
  IbvSetupInformation setupInfo;
  uint64_t memoryRegionPtr;
  uint32_t memoryRegionKey;
};

} // namespace

class Connection::Impl : public std::enable_shared_from_this<Connection::Impl>,
                         public EpollLoop::EventHandler,
                         public IbvEventHandler {
  enum State {
    INITIALIZING = 1,
    SEND_ADDR,
    RECV_ADDR,
    ESTABLISHED,
  };

 public:
  // Create a connection that is already connected (e.g. from a listener).
  Impl(
      std::shared_ptr<Context::PrivateIface> context,
      Socket socket,
      std::string id);

  // Create a connection that connects to the specified address.
  Impl(
      std::shared_ptr<Context::PrivateIface> context,
      std::string addr,
      std::string id);

  // Initialize member fields that need `shared_from_this`.
  void init();

  // Queue a read operation.
  void read(read_callback_fn fn);
  void read(AbstractNopHolder& object, read_nop_callback_fn fn);
  void read(void* ptr, size_t length, read_callback_fn fn);

  // Perform a write operation.
  void write(const void* ptr, size_t length, write_callback_fn fn);
  void write(const AbstractNopHolder& object, write_callback_fn fn);

  // Tell the connection what its identifier is.
  void setId(std::string id);

  // Shut down the connection and its resources.
  void close();

  // Implementation of EventHandler.
  void handleEventsFromLoop(int events) override;

  // Implementation of IbvEventHandler.
  void onRemoteProducedData(uint32_t length) override;
  void onRemoteConsumedData(uint32_t length) override;
  void onWriteCompleted() override;
  void onAckCompleted() override;
  void onError(IbvLib::wc_status status, uint64_t wr_id) override;

 private:
  // Initialize member fields that need `shared_from_this`.
  void initFromLoop();

  // Queue a read operation.
  void readFromLoop(read_callback_fn fn);
  void readFromLoop(AbstractNopHolder& object, read_nop_callback_fn fn);
  void readFromLoop(void* ptr, size_t length, read_callback_fn fn);

  // Perform a write operation.
  void writeFromLoop(const void* ptr, size_t length, write_callback_fn fn);
  void writeFromLoop(const AbstractNopHolder& object, write_callback_fn fn);

  void setIdFromLoop_(std::string id);

  // Shut down the connection and its resources.
  void closeFromLoop();

  // Handle events of type EPOLLIN on the UNIX domain socket.
  //
  // The only data that is expected on that socket is the address and other
  // setup information for the other side's queue pair and inbox.
  void handleEventInFromLoop();

  // Handle events of type EPOLLOUT on the UNIX domain socket.
  //
  // Once the socket is writable we send the address and other setup information
  // for this side's queue pair and inbox.
  void handleEventOutFromLoop();

  State state_{INITIALIZING};
  Error error_{Error::kSuccess};
  std::shared_ptr<Context::PrivateIface> context_;
  Socket socket_;
  optional<Sockaddr> sockaddr_;
  ClosingReceiver closingReceiver_;

  IbvQueuePair qp_;
  IbvSetupInformation ibvSelfInfo_;

  // Inbox.
  // Initialize header during construction because it isn't assignable.
  util::ringbuffer::RingBufferHeader inboxHeader_{kBufferSize};
  // Use mmapped memory so it's page-aligned (and, one day, to use huge pages).
  MmappedPtr inboxBuf_;
  util::ringbuffer::RingBuffer inboxRb_;
  IbvMemoryRegion inboxMr_;

  // Outbox.
  // Initialize header during construction because it isn't assignable.
  util::ringbuffer::RingBufferHeader outboxHeader_{kBufferSize};
  // Use mmapped memory so it's page-aligned (and, one day, to use huge pages).
  MmappedPtr outboxBuf_;
  util::ringbuffer::RingBuffer outboxRb_;
  IbvMemoryRegion outboxMr_;

  // Peer inbox key, pointer and head.
  uint32_t peerInboxKey_{0};
  uint64_t peerInboxPtr_{0};
  uint64_t peerInboxHead_{0};

  // The ringbuffer API is synchronous (it expects data to be consumed/produced
  // immediately "inline" when the buffer is accessed) but InfiniBand is
  // asynchronous, thus we need to abuse the ringbuffer API a bit. When new data
  // is appended to the outbox, we must access it, to send it over IB, but we
  // must first skip over the data that we have already started sending which is
  // still in flight (we can only "commit" that data, by increasing the tail,
  // once the remote acknowledges it, or else it could be overwritten). We keep
  // track of how much data to skip with this field.
  uint32_t numBytesInFlight_{0};

  // The connection performs two types of send requests: writing to the remote
  // inbox, or acknowledging a write into its own inbox. These send operations
  // could be delayed and stalled by the reactor as only a limited number of
  // work requests can be outstanding at the same time globally. Thus we keep
  // count of how many we have pending to make sure they have all completed or
  // flushed when we close, and that none is stuck in the pipeline.
  uint32_t numWritesInFlight_{0};
  uint32_t numAcksInFlight_{0};

  // Pending read operations.
  std::deque<RingbufferReadOperation> readOperations_;

  // Pending write operations.
  std::deque<RingbufferWriteOperation> writeOperations_;

  // A sequence number for the calls to read.
  uint64_t nextBufferBeingRead_{0};

  // A sequence number for the calls to write.
  uint64_t nextBufferBeingWritten_{0};

  // A sequence number for the invocations of the callbacks of read.
  uint64_t nextReadCallbackToCall_{0};

  // A sequence number for the invocations of the callbacks of write.
  uint64_t nextWriteCallbackToCall_{0};

  // An identifier for the connection, composed of the identifier for the
  // context or listener, combined with an increasing sequence number. It will
  // only be used for logging and debugging purposes.
  std::string id_;

  // Process pending read operations if in an operational state.
  //
  // This may be triggered by the other side of the connection (by pushing this
  // side's inbox token to the reactor) when it has written some new data to its
  // outbox (which is this side's inbox). It is also called by this connection
  // when it moves into an established state or when a new read operation is
  // queued, in case data was already available before this connection was ready
  // to consume it.
  void processReadOperationsFromLoop();

  // Process pending write operations if in an operational state.
  //
  // This may be triggered by the other side of the connection (by pushing this
  // side's outbox token to the reactor) when it has read some data from its
  // inbox (which is this side's outbox). This is important when some of this
  // side's writes couldn't complete because the outbox was full, and thus they
  // needed to wait for some of its data to be read. This method is also called
  // by this connection when it moves into an established state, in case some
  // writes were queued before the connection was ready to process them, or when
  // a new write operation is queued.
  void processWriteOperationsFromLoop();

  void setError_(Error error);

  // Deal with an error.
  void handleError();

  void tryCleanup_();
  void cleanup_();
};

Connection::Connection(
    ConstructorToken /* unused */,
    std::shared_ptr<Context::PrivateIface> context,
    Socket socket,
    std::string id)
    : impl_(std::make_shared<Impl>(
          std::move(context),
          std::move(socket),
          std::move(id))) {
  impl_->init();
}

Connection::Connection(
    ConstructorToken /* unused */,
    std::shared_ptr<Context::PrivateIface> context,
    std::string addr,
    std::string id)
    : impl_(std::make_shared<Impl>(
          std::move(context),
          std::move(addr),
          std::move(id))) {
  impl_->init();
}

void Connection::Impl::init() {
  context_->deferToLoop([impl{shared_from_this()}]() { impl->initFromLoop(); });
}

void Connection::close() {
  impl_->close();
}

void Connection::Impl::close() {
  context_->deferToLoop(
      [impl{shared_from_this()}]() { impl->closeFromLoop(); });
}

Connection::~Connection() {
  close();
}

Connection::Impl::Impl(
    std::shared_ptr<Context::PrivateIface> context,
    Socket socket,
    std::string id)
    : context_(std::move(context)),
      socket_(std::move(socket)),
      closingReceiver_(context_, context_->getClosingEmitter()),
      id_(std::move(id)) {}

Connection::Impl::Impl(
    std::shared_ptr<Context::PrivateIface> context,
    std::string addr,
    std::string id)
    : context_(std::move(context)),
      sockaddr_(Sockaddr::createInetSockAddr(addr)),
      closingReceiver_(context_, context_->getClosingEmitter()),
      id_(std::move(id)) {}

void Connection::Impl::initFromLoop() {
  TP_DCHECK(context_->inLoop());

  closingReceiver_.activate(*this);

  Error error;
  // The connection either got a socket or an address, but not both.
  TP_DCHECK(socket_.hasValue() ^ sockaddr_.has_value());
  if (!socket_.hasValue()) {
    std::tie(error, socket_) =
        Socket::createForFamily(sockaddr_->addr()->sa_family);
    if (error) {
      setError_(std::move(error));
      return;
    }
    error = socket_.reuseAddr(true);
    if (error) {
      setError_(std::move(error));
      return;
    }
    error = socket_.connect(sockaddr_.value());
    if (error) {
      setError_(std::move(error));
      return;
    }
  }
  // Ensure underlying control socket is non-blocking such that it
  // works well with event driven I/O.
  error = socket_.block(false);
  if (error) {
    setError_(std::move(error));
    return;
  }

  // Create ringbuffer for inbox.
  inboxBuf_ = MmappedPtr(
      kBufferSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1);
  inboxRb_ = util::ringbuffer::RingBuffer(&inboxHeader_, inboxBuf_.ptr());
  inboxMr_ = createIbvMemoryRegion(
      context_->getReactor().getIbvLib(),
      context_->getReactor().getIbvPd(),
      inboxBuf_.ptr(),
      kBufferSize,
      IbvLib::ACCESS_LOCAL_WRITE | IbvLib::ACCESS_REMOTE_WRITE);

  // Create ringbuffer for outbox.
  outboxBuf_ = MmappedPtr(
      kBufferSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1);
  outboxRb_ = util::ringbuffer::RingBuffer(&outboxHeader_, outboxBuf_.ptr());
  outboxMr_ = createIbvMemoryRegion(
      context_->getReactor().getIbvLib(),
      context_->getReactor().getIbvPd(),
      outboxBuf_.ptr(),
      kBufferSize,
      0);

  // Create and init queue pair.
  {
    IbvLib::qp_init_attr initAttr;
    std::memset(&initAttr, 0, sizeof(initAttr));
    initAttr.qp_type = IbvLib::QPT_RC;
    initAttr.send_cq = context_->getReactor().getIbvCq().get();
    initAttr.recv_cq = context_->getReactor().getIbvCq().get();
    initAttr.cap.max_send_wr = kNumPendingWriteReqs;
    initAttr.cap.max_send_sge = 1;
    initAttr.srq = context_->getReactor().getIbvSrq().get();
    initAttr.sq_sig_all = 1;
    qp_ = createIbvQueuePair(
        context_->getReactor().getIbvLib(),
        context_->getReactor().getIbvPd(),
        initAttr);
  }
  transitionIbvQueuePairToInit(
      context_->getReactor().getIbvLib(),
      qp_,
      context_->getReactor().getIbvAddress());

  // Register methods to be called when our peer writes to our inbox and reads
  // from our outbox.
  context_->getReactor().registerQp(qp_->qp_num, shared_from_this());

  // We're sending address first, so wait for writability.
  state_ = SEND_ADDR;
  context_->registerDescriptor(socket_.fd(), EPOLLOUT, shared_from_this());
}

void Connection::read(read_callback_fn fn) {
  impl_->read(std::move(fn));
}

void Connection::Impl::read(read_callback_fn fn) {
  context_->deferToLoop(
      [impl{shared_from_this()}, fn{std::move(fn)}]() mutable {
        impl->readFromLoop(std::move(fn));
      });
}

void Connection::Impl::readFromLoop(read_callback_fn fn) {
  TP_DCHECK(context_->inLoop());

  uint64_t sequenceNumber = nextBufferBeingRead_++;
  TP_VLOG(7) << "Connection " << id_ << " received an unsized read request (#"
             << sequenceNumber << ")";

  fn = [this, sequenceNumber, fn{std::move(fn)}](
           const Error& error, const void* ptr, size_t length) {
    TP_DCHECK_EQ(sequenceNumber, nextReadCallbackToCall_++);
    TP_VLOG(7) << "Connection " << id_
               << " is calling an unsized read callback (#" << sequenceNumber
               << ")";
    fn(error, ptr, length);
    TP_VLOG(7) << "Connection " << id_
               << " done calling an unsized read callback (#" << sequenceNumber
               << ")";
  };

  if (error_) {
    fn(error_, nullptr, 0);
    return;
  }

  readOperations_.emplace_back(std::move(fn));

  // If the inbox already contains some data, we may be able to process this
  // operation right away.
  processReadOperationsFromLoop();
}

void Connection::read(AbstractNopHolder& object, read_nop_callback_fn fn) {
  impl_->read(object, std::move(fn));
}

void Connection::Impl::read(
    AbstractNopHolder& object,
    read_nop_callback_fn fn) {
  context_->deferToLoop(
      [impl{shared_from_this()}, &object, fn{std::move(fn)}]() mutable {
        impl->readFromLoop(object, std::move(fn));
      });
}

void Connection::Impl::readFromLoop(
    AbstractNopHolder& object,
    read_nop_callback_fn fn) {
  TP_DCHECK(context_->inLoop());

  uint64_t sequenceNumber = nextBufferBeingRead_++;
  TP_VLOG(7) << "Connection " << id_ << " received a nop object read request (#"
             << sequenceNumber << ")";

  fn = [this, sequenceNumber, fn{std::move(fn)}](const Error& error) {
    TP_DCHECK_EQ(sequenceNumber, nextReadCallbackToCall_++);
    TP_VLOG(7) << "Connection " << id_
               << " is calling a nop object read callback (#" << sequenceNumber
               << ")";
    fn(error);
    TP_VLOG(7) << "Connection " << id_
               << " done calling a nop object read callback (#"
               << sequenceNumber << ")";
  };

  if (error_) {
    fn(error_);
    return;
  }

  readOperations_.emplace_back(
      &object,
      [fn{std::move(fn)}](
          const Error& error, const void* /* unused */, size_t /* unused */) {
        fn(error);
      });

  // If the inbox already contains some data, we may be able to process this
  // operation right away.
  processReadOperationsFromLoop();
}

void Connection::read(void* ptr, size_t length, read_callback_fn fn) {
  impl_->read(ptr, length, std::move(fn));
}

void Connection::Impl::read(void* ptr, size_t length, read_callback_fn fn) {
  context_->deferToLoop(
      [impl{shared_from_this()}, ptr, length, fn{std::move(fn)}]() mutable {
        impl->readFromLoop(ptr, length, std::move(fn));
      });
}

void Connection::Impl::readFromLoop(
    void* ptr,
    size_t length,
    read_callback_fn fn) {
  TP_DCHECK(context_->inLoop());

  uint64_t sequenceNumber = nextBufferBeingRead_++;
  TP_VLOG(7) << "Connection " << id_ << " received a sized read request (#"
             << sequenceNumber << ")";

  fn = [this, sequenceNumber, fn{std::move(fn)}](
           const Error& error, const void* ptr, size_t length) {
    TP_DCHECK_EQ(sequenceNumber, nextReadCallbackToCall_++);
    TP_VLOG(7) << "Connection " << id_ << " is calling a sized read callback (#"
               << sequenceNumber << ")";
    fn(error, ptr, length);
    TP_VLOG(7) << "Connection " << id_
               << " done calling a sized read callback (#" << sequenceNumber
               << ")";
  };

  if (error_) {
    fn(error_, ptr, length);
    return;
  }

  readOperations_.emplace_back(ptr, length, std::move(fn));

  // If the inbox already contains some data, we may be able to process this
  // operation right away.
  processReadOperationsFromLoop();
}

void Connection::write(const void* ptr, size_t length, write_callback_fn fn) {
  impl_->write(ptr, length, std::move(fn));
}

void Connection::Impl::write(
    const void* ptr,
    size_t length,
    write_callback_fn fn) {
  context_->deferToLoop(
      [impl{shared_from_this()}, ptr, length, fn{std::move(fn)}]() mutable {
        impl->writeFromLoop(ptr, length, std::move(fn));
      });
}

void Connection::Impl::writeFromLoop(
    const void* ptr,
    size_t length,
    write_callback_fn fn) {
  TP_DCHECK(context_->inLoop());

  uint64_t sequenceNumber = nextBufferBeingWritten_++;
  TP_VLOG(7) << "Connection " << id_ << " received a write request (#"
             << sequenceNumber << ")";

  fn = [this, sequenceNumber, fn{std::move(fn)}](const Error& error) {
    TP_DCHECK_EQ(sequenceNumber, nextWriteCallbackToCall_++);
    TP_VLOG(7) << "Connection " << id_ << " is calling a write callback (#"
               << sequenceNumber << ")";
    fn(error);
    TP_VLOG(7) << "Connection " << id_ << " done calling a write callback (#"
               << sequenceNumber << ")";
  };

  if (error_) {
    fn(error_);
    return;
  }

  writeOperations_.emplace_back(ptr, length, std::move(fn));

  // If the outbox has some free space, we may be able to process this operation
  // right away.
  processWriteOperationsFromLoop();
}

void Connection::write(const AbstractNopHolder& object, write_callback_fn fn) {
  impl_->write(object, std::move(fn));
}

void Connection::Impl::write(
    const AbstractNopHolder& object,
    write_callback_fn fn) {
  context_->deferToLoop(
      [impl{shared_from_this()}, &object, fn{std::move(fn)}]() mutable {
        impl->writeFromLoop(object, std::move(fn));
      });
}

void Connection::Impl::writeFromLoop(
    const AbstractNopHolder& object,
    write_callback_fn fn) {
  TP_DCHECK(context_->inLoop());

  uint64_t sequenceNumber = nextBufferBeingWritten_++;
  TP_VLOG(7) << "Connection " << id_
             << " received a nop object write request (#" << sequenceNumber
             << ")";

  fn = [this, sequenceNumber, fn{std::move(fn)}](const Error& error) {
    TP_DCHECK_EQ(sequenceNumber, nextWriteCallbackToCall_++);
    TP_VLOG(7) << "Connection " << id_
               << " is calling a nop object write callback (#" << sequenceNumber
               << ")";
    fn(error);
    TP_VLOG(7) << "Connection " << id_
               << " done calling a nop object write callback (#"
               << sequenceNumber << ")";
  };

  if (error_) {
    fn(error_);
    return;
  }

  writeOperations_.emplace_back(&object, std::move(fn));

  // If the outbox has some free space, we may be able to process this operation
  // right away.
  processWriteOperationsFromLoop();
}

void Connection::setId(std::string id) {
  impl_->setId(std::move(id));
}

void Connection::Impl::setId(std::string id) {
  context_->deferToLoop(
      [impl{shared_from_this()}, id{std::move(id)}]() mutable {
        impl->setIdFromLoop_(std::move(id));
      });
}

void Connection::Impl::setIdFromLoop_(std::string id) {
  TP_DCHECK(context_->inLoop());
  TP_VLOG(7) << "Connection " << id_ << " was renamed to " << id;
  id_ = std::move(id);
}

void Connection::Impl::handleEventsFromLoop(int events) {
  TP_DCHECK(context_->inLoop());
  TP_VLOG(9) << "Connection " << id_ << " is handling an event on its socket ("
             << EpollLoop::formatEpollEvents(events) << ")";

  // Handle only one of the events in the mask. Events on the control
  // file descriptor are rare enough for the cost of having epoll call
  // into this function multiple times to not matter. The benefit is
  // that every handler can close and unregister the control file
  // descriptor from the event loop, without worrying about the next
  // handler trying to do so as well.
  // In some cases the socket could be in a state where it's both in an error
  // state and readable/writable. If we checked for EPOLLIN or EPOLLOUT first
  // and then returned after handling them, we would keep doing so forever and
  // never reach the error handling. So we should keep the error check first.
  if (events & EPOLLERR) {
    int error;
    socklen_t errorlen = sizeof(error);
    int rv = getsockopt(
        socket_.fd(),
        SOL_SOCKET,
        SO_ERROR,
        reinterpret_cast<void*>(&error),
        &errorlen);
    if (rv == -1) {
      setError_(TP_CREATE_ERROR(SystemError, "getsockopt", rv));
    } else {
      setError_(TP_CREATE_ERROR(SystemError, "async error on socket", error));
    }
    return;
  }
  if (events & EPOLLIN) {
    handleEventInFromLoop();
    return;
  }
  if (events & EPOLLOUT) {
    handleEventOutFromLoop();
    return;
  }
  // Check for hangup last, as there could be cases where we get EPOLLHUP but
  // there's still data to be read from the socket, so we want to deal with that
  // before dealing with the hangup.
  if (events & EPOLLHUP) {
    setError_(TP_CREATE_ERROR(EOFError));
    return;
  }
}

void Connection::Impl::handleEventInFromLoop() {
  TP_DCHECK(context_->inLoop());
  if (state_ == RECV_ADDR) {
    struct Exchange ex;

    auto err = socket_.read(&ex, sizeof(ex));
    // Crossing our fingers that the exchange information is small enough that
    // it can be read in a single chunk.
    if (err != sizeof(ex)) {
      setError_(TP_CREATE_ERROR(ShortReadError, sizeof(ex), err));
      return;
    }

    transitionIbvQueuePairToReadyToReceive(
        context_->getReactor().getIbvLib(),
        qp_,
        context_->getReactor().getIbvAddress(),
        ex.setupInfo);
    transitionIbvQueuePairToReadyToSend(
        context_->getReactor().getIbvLib(), qp_, ibvSelfInfo_);

    peerInboxKey_ = ex.memoryRegionKey;
    peerInboxPtr_ = ex.memoryRegionPtr;

    // The connection is usable now.
    state_ = ESTABLISHED;
    processWriteOperationsFromLoop();
    // Trigger read operations in case a pair of local read() and remote
    // write() happened before connection is established. Otherwise read()
    // callback would lose if it's the only read() request.
    processReadOperationsFromLoop();
    return;
  }

  if (state_ == ESTABLISHED) {
    // We don't expect to read anything on this socket once the
    // connection has been established. If we do, assume it's a
    // zero-byte read indicating EOF.
    setError_(TP_CREATE_ERROR(EOFError));
    return;
  }

  TP_THROW_ASSERT() << "EPOLLIN event not handled in state " << state_;
}

void Connection::Impl::handleEventOutFromLoop() {
  TP_DCHECK(context_->inLoop());
  if (state_ == SEND_ADDR) {
    Exchange ex;
    ibvSelfInfo_ =
        makeIbvSetupInformation(context_->getReactor().getIbvAddress(), qp_);
    ex.setupInfo = ibvSelfInfo_;
    ex.memoryRegionPtr = reinterpret_cast<uint64_t>(inboxBuf_.ptr());
    ex.memoryRegionKey = inboxMr_->rkey;

    auto err = socket_.write(reinterpret_cast<void*>(&ex), sizeof(ex));
    // Crossing our fingers that the exchange information is small enough that
    // it can be written in a single chunk.
    if (err != sizeof(ex)) {
      setError_(TP_CREATE_ERROR(ShortWriteError, sizeof(ex), err));
      return;
    }

    // Sent our address. Wait for address from peer.
    state_ = RECV_ADDR;
    context_->registerDescriptor(socket_.fd(), EPOLLIN, shared_from_this());
    return;
  }

  TP_THROW_ASSERT() << "EPOLLOUT event not handled in state " << state_;
}

void Connection::Impl::processReadOperationsFromLoop() {
  TP_DCHECK(context_->inLoop());

  // Process all read read operations that we can immediately serve, only
  // when connection is established.
  if (state_ != ESTABLISHED) {
    return;
  }
  // Serve read operations
  util::ringbuffer::Consumer inboxConsumer(inboxRb_);
  while (!readOperations_.empty()) {
    RingbufferReadOperation& readOperation = readOperations_.front();
    ssize_t len = readOperation.handleRead(inboxConsumer);
    if (len > 0) {
      IbvLib::send_wr wr;
      std::memset(&wr, 0, sizeof(wr));
      wr.wr_id = kAckRequestId;
      wr.opcode = IbvLib::WR_SEND_WITH_IMM;
      wr.imm_data = len;

      TP_VLOG(9) << "Connection " << id_
                 << " is posting a send request (acknowledging " << wr.imm_data
                 << " bytes) on QP " << qp_->qp_num;
      context_->getReactor().postAck(qp_, wr);
      numAcksInFlight_++;
    }
    if (readOperation.completed()) {
      readOperations_.pop_front();
    } else {
      break;
    }
  }
}

void Connection::Impl::processWriteOperationsFromLoop() {
  TP_DCHECK(context_->inLoop());

  if (state_ != ESTABLISHED) {
    return;
  }

  util::ringbuffer::Producer outboxProducer(outboxRb_);
  while (!writeOperations_.empty()) {
    RingbufferWriteOperation& writeOperation = writeOperations_.front();
    ssize_t len = writeOperation.handleWrite(outboxProducer);
    if (len > 0) {
      ssize_t ret;
      util::ringbuffer::Consumer outboxConsumer(outboxRb_);

      // In order to get the pointers and lengths to the data that was just
      // written to the ringbuffer we pretend to start a consumer transaction so
      // we can use accessContiguous, which we'll however later abort. The data
      // will only be really consumed once we receive the ACK from the remote.

      ret = outboxConsumer.startTx();
      TP_THROW_SYSTEM_IF(ret < 0, -ret);

      ssize_t numBuffers;
      std::array<util::ringbuffer::Consumer::Buffer, 2> buffers;

      // Skip over the data that was already sent but is still in flight.
      std::tie(numBuffers, buffers) =
          outboxConsumer.accessContiguousInTx</*allowPartial=*/false>(
              numBytesInFlight_);

      std::tie(numBuffers, buffers) =
          outboxConsumer.accessContiguousInTx</*allowPartial=*/false>(len);
      TP_THROW_SYSTEM_IF(numBuffers < 0, -numBuffers);

      for (int bufferIdx = 0; bufferIdx < numBuffers; bufferIdx++) {
        IbvLib::sge list;
        list.addr = reinterpret_cast<uint64_t>(buffers[bufferIdx].ptr);
        list.length = buffers[bufferIdx].len;
        list.lkey = outboxMr_->lkey;

        uint64_t peerInboxOffset = peerInboxHead_ & (kBufferSize - 1);
        peerInboxHead_ += buffers[bufferIdx].len;

        IbvLib::send_wr wr;
        std::memset(&wr, 0, sizeof(wr));
        wr.wr_id = kWriteRequestId;
        wr.sg_list = &list;
        wr.num_sge = 1;
        wr.opcode = IbvLib::WR_RDMA_WRITE_WITH_IMM;
        wr.imm_data = buffers[bufferIdx].len;
        wr.wr.rdma.remote_addr = peerInboxPtr_ + peerInboxOffset;
        wr.wr.rdma.rkey = peerInboxKey_;

        TP_VLOG(9) << "Connection " << id_
                   << " is posting a RDMA write request (transmitting "
                   << wr.imm_data << " bytes) on QP " << qp_->qp_num;
        context_->getReactor().postWrite(qp_, wr);
        numWritesInFlight_++;
      }

      ret = outboxConsumer.cancelTx();
      TP_THROW_SYSTEM_IF(ret < 0, -ret);

      numBytesInFlight_ += len;
    }
    if (writeOperation.completed()) {
      writeOperations_.pop_front();
    } else {
      break;
    }
  }
}

void Connection::Impl::onRemoteProducedData(uint32_t length) {
  TP_DCHECK(context_->inLoop());
  TP_VLOG(9) << "Connection " << id_ << " was signalled that " << length
             << " bytes were written to its inbox on QP " << qp_->qp_num;
  // We could start a transaction and use the proper methods for this, but as
  // this method is the only producer for the inbox ringbuffer we can cut it
  // short and directly increase the head.
  inboxHeader_.incHead(length);
  processReadOperationsFromLoop();
}

void Connection::Impl::onRemoteConsumedData(uint32_t length) {
  TP_DCHECK(context_->inLoop());
  TP_VLOG(9) << "Connection " << id_ << " was signalled that " << length
             << " bytes were read from its outbox on QP " << qp_->qp_num;
  // We could start a transaction and use the proper methods for this, but as
  // this method is the only consumer for the outbox ringbuffer we can cut it
  // short and directly increase the tail.
  outboxHeader_.incTail(length);
  numBytesInFlight_ -= length;
  processWriteOperationsFromLoop();
}

void Connection::Impl::onWriteCompleted() {
  TP_DCHECK(context_->inLoop());
  TP_VLOG(9) << "Connection " << id_
             << " done posting a RDMA write request on QP " << qp_->qp_num;
  numWritesInFlight_--;
  tryCleanup_();
}

void Connection::Impl::onAckCompleted() {
  TP_DCHECK(context_->inLoop());
  TP_VLOG(9) << "Connection " << id_ << " done posting a send request on QP "
             << qp_->qp_num;
  numAcksInFlight_--;
  tryCleanup_();
}

void Connection::Impl::onError(IbvLib::wc_status status, uint64_t wr_id) {
  TP_DCHECK(context_->inLoop());
  setError_(TP_CREATE_ERROR(
      IbvError, context_->getReactor().getIbvLib().wc_status_str(status)));
  if (wr_id == kWriteRequestId) {
    onWriteCompleted();
  } else if (wr_id == kAckRequestId) {
    onAckCompleted();
  }
}

void Connection::Impl::setError_(Error error) {
  // Don't overwrite an error that's already set.
  if (error_ || !error) {
    return;
  }

  error_ = std::move(error);

  handleError();
}

void Connection::Impl::handleError() {
  TP_DCHECK(context_->inLoop());
  TP_VLOG(8) << "Connection " << id_ << " is handling error " << error_.what();

  for (auto& readOperation : readOperations_) {
    readOperation.handleError(error_);
  }
  readOperations_.clear();
  for (auto& writeOperation : writeOperations_) {
    writeOperation.handleError(error_);
  }
  writeOperations_.clear();

  transitionIbvQueuePairToError(context_->getReactor().getIbvLib(), qp_);

  tryCleanup_();

  if (socket_.hasValue()) {
    if (state_ > INITIALIZING) {
      context_->unregisterDescriptor(socket_.fd());
    }
    socket_.reset();
  }
}

void Connection::Impl::tryCleanup_() {
  TP_DCHECK(context_->inLoop());
  // Setting the queue pair to an error state will cause all its work requests
  // (both those that had started being served, and those that hadn't; including
  // those from a shared receive queue) to be flushed. We need to wait for the
  // completion events of all those requests to be retrieved from the completion
  // queue before we can destroy the queue pair. We can do so by deferring the
  // destruction to the loop, since the reactor will only proceed to invoke
  // deferred functions once it doesn't have any completion events to handle.
  // However the RDMA writes and the sends may be queued up inside the reactor
  // and thus may not have even been scheduled yet, so we explicitly wait for
  // them to complete.
  if (error_) {
    if (numWritesInFlight_ == 0 && numAcksInFlight_ == 0) {
      TP_VLOG(8) << "Connection " << id_ << " is ready to clean up";
      context_->deferToLoop([impl{shared_from_this()}]() { impl->cleanup_(); });
    } else {
      TP_VLOG(9) << "Connection " << id_
                 << " cannot proceed to cleanup because it has "
                 << numWritesInFlight_ << " pending RDMA write requests and "
                 << numAcksInFlight_ << " pending send requests on QP "
                 << qp_->qp_num;
    }
  }
}

void Connection::Impl::cleanup_() {
  TP_DCHECK(context_->inLoop());
  TP_VLOG(8) << "Connection " << id_ << " is cleaning up";

  context_->getReactor().unregisterQp(qp_->qp_num);

  qp_.reset();
  inboxMr_.reset();
  inboxBuf_.reset();
  outboxMr_.reset();
  outboxBuf_.reset();
}

void Connection::Impl::closeFromLoop() {
  TP_DCHECK(context_->inLoop());
  TP_VLOG(7) << "Connection " << id_ << " is closing";
  setError_(TP_CREATE_ERROR(ConnectionClosedError));
}

} // namespace ibv
} // namespace transport
} // namespace tensorpipe
