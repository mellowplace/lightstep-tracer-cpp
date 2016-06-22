#include <exception>
#include <thread>
#include <iostream>  // TODO Remove in favor of error logging support.

#include "impl.h"
#include "recorder.h"
#include "options.h"
#include "util.h"

namespace lightstep {

using namespace lightstep_net;

const char CollectorThriftRpcPath[] = "/_rpc/v1/reports/binary";

const uint64_t MaxSpansPerReport = 2500;
const uint64_t ReportIntervalMillisecs = 2500;
const uint32_t ReportSizeLowerBound = 200;

namespace {

class Initializer {
public:
  Initializer() {
    RegisterRecorderFactory([](const TracerImpl& impl) {
	return NewDefaultRecorder(impl);
      });
  }
};

// Registers a factory.
Initializer initializer;

}  // namespace

namespace transport {

typedef std::lock_guard<std::mutex> MutexLock;

class DefaultRecorder : public Recorder {
public:
  DefaultRecorder(const TracerImpl& impl);
  ~DefaultRecorder();

  void RecordSpan(lightstep_net::SpanRecord&& span) override;

  void Flush() override;
  
private:
  void Write();

  void ProcessResponse(const ReportResponse& response);

  // Forces the writer thread to exit immediately.
  void make_writer_exit() {
    MutexLock lock(cond_mutex_);
    writer_exit_ = true;
    writer_cond_.notify_one();
  }

  // Waits until either the timeout or the writer thread is forced to
  // exit.  Returns true if it should continue writing, false if it
  // should exit.
  bool wait_to_write_until(const std::chrono::steady_clock::time_point &next) {
    std::unique_lock<std::mutex> lock(cond_mutex_);
    writer_cond_.wait_until(lock, next, [this]() {
	return this->writer_exit_;
      });
    return !writer_exit_;
  }
    
  const TracerImpl& tracer_;

  std::mutex cond_mutex_; // For the writer condition variable
  std::mutex flush_mutex_; // Prevent simultaneous flushes
  std::mutex pending_mutex_; // Protect pending_spans

  // Background Flush thread.
  std::thread writer_;
  std::condition_variable writer_cond_;
  bool writer_exit_;

  // Vector of spans for the current/last flush.  This is swapped with
  // pending_spans_ at each flush.
  std::vector<lightstep_net::SpanRecord> flushing_spans_;

  // Vector of spans for the next flush
  std::vector<lightstep_net::SpanRecord> pending_spans_;

  // Transport mechanism.
  // boost::shared_ptr<TSocket> socket_;
  // boost::shared_ptr<TSSLSocketFactory> ssl_factory_;
  // boost::shared_ptr<THttpClient> transport_;
};

DefaultRecorder::DefaultRecorder(const TracerImpl& tracer)
  : tracer_(tracer),
    writer_exit_(false) {
  MutexLock lock(flush_mutex_);

  writer_ = std::thread(&DefaultRecorder::Write, this);

  std::cerr << "Connecting..." << tracer_.options().collector_host << ":" << tracer_.options().collector_port << CollectorThriftRpcPath;

  socket_ = boost::shared_ptr<TSocket>(new TSocket(tracer_.options().collector_host,
						   tracer_.options().collector_port));
  boost::shared_ptr<TTransport> trans = socket_;

  if (tracer_.options().collector_encryption == "tls") {
    initializeOpenSSL();
    ssl_factory_ = boost::shared_ptr<TSSLSocketFactory>(new TSSLSocketFactory(SSLTLS));
    trans = ssl_factory_->createSocket(socket_->getSocketFD());
  }

  // Note: THttpClient buffers the input and output automatically.
  transport_ = boost::shared_ptr<THttpClient>(new THttpClient(trans, tracer_.options().collector_host, CollectorThriftRpcPath));

  try {
    transport_->open();
  } catch (std::exception &e) {
    std::cerr << "Connect error: " << e.what() << std::endl;
  }
}

DefaultRecorder::~DefaultRecorder() {
  make_writer_exit();
  writer_.join();
}

void DefaultRecorder::RecordSpan(lightstep_net::SpanRecord&& span) {
  MutexLock lock(pending_mutex_);
  if (pending_spans_.size() < MaxSpansPerReport) {
    pending_spans_.emplace_back(std::move(span));
  } else {
    // TODO Count dropped span.
  }
}

void DefaultRecorder::Write() {
  using namespace std::chrono;
  auto interval = milliseconds(ReportIntervalMillisecs);
  auto next = steady_clock::now() + interval;

  while (wait_to_write_until(next)) {
    Flush();

    auto end = steady_clock::now();
    auto elapsed = end - next;

    if (elapsed > interval) {
      next = end;
    } else {
      next = end + interval - elapsed;
    }    
  }
}

void DefaultRecorder::Flush() {
  MutexLock lock(flush_mutex_);
  {
    MutexLock lock(pending_mutex_);

    // Note: At present, there cannot be another Flush call
    // outstanding because the Flush API is not exposed.  More
    // synchronization will be needed if flushing_spans_ is pending in
    // another Flush operation.
    if (!flushing_spans_.empty()) {
      throw std::runtime_error("Invalid flushing_spans_ state");
    }
    std::swap(flushing_spans_, pending_spans_);

    if (flushing_spans_.empty()) return;
  }

  EncodeForTransit(tracer_, flushing_spans_, [this](const uint8_t* buffer, uint32_t length) {
      try {
	if (!transport_->isOpen()) {
	  transport_->open();
	}
	transport_->write(buffer, length);
	transport_->flush();

	// Note: Use Thrift directly here to read a response because
	// it's difficult to know how much to read without making
	// assumptions about the underlying transport.
	//
	// Recommend for "DIY" transport to ignore the response and
	// simply drain the response channel.  We will abandon Thrift
	// for transport in favor of gRPC, after which this topic will
	// be revisited.
	boost::shared_ptr<TBinaryProtocol> proto(new TBinaryProtocol(transport_));
	ReportingServiceClient client(proto);

	ReportResponse response;
	client.recv_Report(response);
	ProcessResponse(response);
      } catch (std::exception &e) {
	std::cerr << "Write error: " << e.what() << std::endl;
      }	
    });
}

void DefaultRecorder::ProcessResponse(const ReportResponse& response) {
  // TODO Implement clock synchronization.  (Though we think that if
  // this code is running on a machine with NTP, shouldn't be
  // necessary.)
  //
  // TODO Implement Disable command ("Big Red Button") support.
  for (const auto& error : response.errors) {
    if (error.empty()) continue;
    // TODO Use proper logging.
    std::cerr << "Controller error: " << error;
    if (error[error.size()-1] != '\n') {
      std::cerr << std::endl;
    }
  }
}

} // namespace transport

std::unique_ptr<Recorder> NewDefaultRecorder(const TracerImpl& impl) {
  return std::unique_ptr<Recorder>(new transport::DefaultRecorder(impl));
}

void Recorder::EncodeForTransit(const TracerImpl& tracer,
				std::vector<lightstep_net::SpanRecord>& spans,
				std::function<void(const uint8_t* bytes, uint32_t len)> func) {

  // TODO Use a more accurate size upper-bound.
  uint32_t size_est = ReportSizeLowerBound * spans.size();
  boost::shared_ptr<TMemoryBuffer> memory(new TMemoryBuffer(size_est));
  boost::shared_ptr<TBinaryProtocol> proto(new TBinaryProtocol(memory));

  ReportRequest report;

  // Note: the following construction happens once per report but
  // is static information, could be precomputed.
  Runtime runtime;
  runtime.__set_guid(tracer.runtime_guid());
  runtime.__set_start_micros(tracer.runtime_start_micros());
  runtime.__set_group_name(tracer.component_name());
  std::vector<KeyValue> attrs;
  for (auto it = tracer.runtime_attributes().begin();
       it != tracer.runtime_attributes().end(); ++it) {
    attrs.emplace_back(util::make_kv(it->first, it->second));
  }
  runtime.attrs = std::move(attrs);
  runtime.__isset.attrs = true;
  report.runtime = std::move(runtime);
  report.__isset.runtime = true;

  // Swap the flushing spans into report, serialize, swap back, then
  // clear.  This allows re-use of the spans memory.
  std::swap(report.span_records, spans);
  report.__isset.span_records = true;

  lightstep_net::ReportingServiceClient client(proto);
  lightstep_net::Auth auth;
  auth.__set_access_token(tracer.access_token());
  client.send_Report(auth, report);

  std::swap(report.span_records, spans);
  spans.clear();

  uint8_t *buffer;
  uint32_t length;
  memory->getBuffer(&buffer, &length);

  func(buffer, length);
}

}  // namespace lightstep
