#include "source/common/router/router_http_cache.h"

#include <chrono>
#include <cstddef>
#include <list>
#include <string>
#include <utility>
#include <vector>

#include "envoy/buffer/buffer.h"
#include "envoy/common/platform.h"
#include "envoy/event/deferred_deletable.h"
#include "envoy/event/dispatcher.h"
#include "envoy/http/async_client.h"
#include "envoy/http/filter.h"
#include "envoy/http/header_map.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/http/headers.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/singleton/threadsafe_singleton.h"

#include "absl/hash/hash.h"
#include "absl/synchronization/mutex.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Http {
namespace Cache {

// CacheServedRequest

CacheServedRequest::CacheServedRequest(CacheEntry* parent, StreamDecoderFilterCallbacks* callbacks, bool head_request) {
  parent_ = parent;
  callbacks_ = callbacks;
  head_request_ = head_request;
  buffer_ = callbacks->dispatcher().getWatermarkFactory().createBuffer([]() -> void {}, 
                                                                       []() -> void {}, 
                                                                       []() -> void {});
}

CacheServedRequest::~CacheServedRequest() {
  if (parent_) {
    absl::MutexLock lock(&parent_->mtx_);
    if (schedulable_event_->enabled()) schedulable_event_->cancel();
    parent_->events_.erase(event_it_);
  }
}

void CacheServedRequest::schedule() {
  if (do_schedule_) {
    do_schedule_ = false; 
    schedulable_event_->scheduleCallbackCurrentIteration();
  }
}

void CacheServedRequest::serve() {
  if (parent_->isCacheable()) {
    bool ended_stream = parent_->reply(callbacks_, *buffer_, start_, head_request_);
    bool do_close_stream = false;
    {
      absl::MutexLock lock(&parent_->mtx_);
      if (parent_->is_completed_ || (start_ == 0 && head_request_ == true)) { 
        do_close_stream = !ended_stream;
      } 
      else { // request not completed yet
        do_schedule_ = true; // schedule this again
        return;
      }
    }
    // we avoid using a mutex in the reply so the request could have gotten finished while
    // we got all the data, but we didn't finish the stream (should be a rare occurence)
    if (do_close_stream) parent_->reply(callbacks_, *buffer_, start_, head_request_);
  } 
  else { // response is not cacheable
    // this should start the request anew
    // a probably better alternative would be to call router::DecodeHeaders again
    callbacks_->recreateStream(nullptr); 
  }
}

// CacheEntry

CacheEntry::~CacheEntry() {
  for (auto event_it = events_.begin(); event_it != events_.end(); ++event_it) {
    // should probably do this with the mtx locked to stay even safer
    (*event_it)->parent_ = nullptr; // in case this gets somehow deleted before router
  }
  delete next_;
}

bool CacheEntry::isSame(RequestHeaderMap& headers) {
  return host_ == headers.getHostValue() && path_ == headers.getPathValue();
}

bool CacheEntry::isCacheable() { 
  return cacheable_; 
}

// this should only be called with the mutex already locked
void CacheEntry::addCallbackToDispatcher(StreamDecoderFilterCallbacks* callbacks, bool head_request, Event::DeferredDeletablePtr& dest) {
  dest = std::make_unique<CacheServedRequest>(this, callbacks, head_request);
  // this is wrong from the point of view that the pointer isn't unique
  // but this reference gets removed in the destructor so it is safe as long as
  // it runs on the same thread which it should (same dispatcher)
  events_.push_front(dynamic_cast<CacheServedRequest*>(&*dest));
  auto event_it = events_.begin();
  auto& event = **event_it;
  // use a function to do this
  event.event_it_ = event_it;
  // use a lambda for the callback because CacheServedRequest isn't copy constructible (unique_ptr)
  // and so we can't use it here as a functor itself (also we want to keep ownership of it)
  event.schedulable_event_ = callbacks->dispatcher().createSchedulableCallback([&event]() -> void { event.serve(); });
}

void CacheEntry::assign(RequestHeaderMap& headers) {
  host_ = {headers.getHostValue().begin(), headers.getHostValue().end()};
  path_ = {headers.getPathValue().begin(), headers.getPathValue().end()};
  is_empty_ = false;
}

void CacheEntry::startRequest(RequestHeaderMap& headers, AsyncClient& client) {
  auto options = Http::AsyncClient::RequestOptions()
                 // just some random settings I decided on, should be configurable
                 .setTimeout(std::chrono::milliseconds(5000))
                 .setSampled(false)
                 .setIsShadow(false)
                 .setBufferLimit(0);
  stream_ = client.start(*this, options);
  // possibly better to adjust the headers more
  cache_request_headers_ = createHeaderMap<RequestHeaderMapImpl>(headers);
  cache_request_headers_->setMethod(Http::Headers::get().MethodValues.Get);
  cache_request_headers_->remove(Http::LowerCaseString("range"));
  if (stream_) 
    stream_->sendHeaders(*cache_request_headers_, true);
  else
    cacheable_ = false; // failed to create a stream so do a panic fallback to noncacheable
}

void CacheEntry::onHeaders(ResponseHeaderMapPtr&& headers, bool end_stream) {
  // some simple clumsy check if we received headers preventing us from caching the response
  // this is only for upstream headers, similar check should be done for downstream headers
  auto cc_headers = headers->get(Http::LowerCaseString("cache-control"));
  if (cc_headers.size()) {
    auto lower_cased_cc = Http::LowerCaseString(cc_headers[0]->value().getStringView());
    auto cache_control = absl::string_view(lower_cased_cc);
    cacheable_ &= (cache_control.find("no-cache") == absl::string_view::npos &&
                   cache_control.find("no-store") == absl::string_view::npos &&
                   cache_control.find("private")  == absl::string_view::npos);
  }
  auto vary_headers = headers->get(Http::LowerCaseString("vary"));
  if (vary_headers.size()) {
    cacheable_ &= (vary_headers[0]->value().getStringView() != "*");
  }
  if (cacheable_) {
    // here we should probably alter the headers a bit for storage (e.g. discard client specific headers)
    response_headers_ = std::move(headers);
    if (end_stream) is_completed_ = true; // is_completed_ = end_stream is probably safe as well
  }
  scheduleCallbacks();
}

void CacheEntry::onData(Buffer::Instance& data, bool end_stream) {
  std::vector<char> tmp(data.length());
  data.copyOut(0, data.length(), static_cast<void*>(&tmp[0]));
  data.drain(data.length());
  data_.emplace_back(std::move(tmp));
  ++data_num_; // data is safely in place so we can increase it without a mutex
  if (end_stream) is_completed_ = true; // is_completed_ = end_stream is probably safe as well
  scheduleCallbacks();
}

bool CacheEntry::reply(StreamDecoderFilterCallbacks* callbacks, Buffer::Instance& buffer, int& start, bool head_request) {
  bool end_stream = false;
  if (start == -1) {
    // send headers
    end_stream = (data_.size() == 0 && is_completed_) || head_request;
    callbacks->encodeHeaders(createHeaderMap<ResponseHeaderMapImpl>(*response_headers_),
                             end_stream, response_headers_->getStatusValue());
    start = 0;
  }
  if (!end_stream) {
    // send data
    int read = 0;
    // this could use some optimization, and a redesign to support range requests and watermarking
    for (auto it = data_.begin(); read < data_num_; ++it, ++read) {
      if (start == read) {
        auto fragment = new Buffer::BufferFragmentImpl(static_cast<void*>(&*it->begin()),it->size(),
          [](const void*, size_t, const Buffer::BufferFragmentImpl* buf_ptr) -> void {
          delete buf_ptr;
        });
        buffer.addBufferFragment(*fragment);
        ++start;
      }
    }
    end_stream = is_completed_ && start == data_num_;
    callbacks->encodeData(buffer, end_stream); // still not perfect condition for end_stream
  }
  return end_stream;
}

void CacheEntry::onComplete() {
  is_completed_ = true;
  scheduleCallbacks();
}

void CacheEntry::scheduleCallbacks() {
  // This should probably be done without a mutex since if a big herd was still coming in
  // all threads could get locked on this mutex while we are waking up a potentially big
  // number of events. A solution would probably be to get the number of events at the start
  // and only wake up those. Then when onComplete() gets called schedule an extra event for
  // current iteration that would do the waking with a mutex.
  // for now to make sure every callback gets executed we use a mutex
  absl::MutexLock lock(&mtx_);
  // go from the back because we add to the front (easier to use iterators for erase() function)
  for (auto event_it = events_.rbegin(); event_it != events_.rend(); ++event_it) {
    (*event_it)->schedule();
  }
}

// SimpleCache

std::size_t SimpleCache::getHash(RequestHeaderMap& headers) {
  // this would get more intricate based on vary headers
  absl::Hash<absl::string_view> hasher{};
  return (hasher(headers.getHostValue()) ^ // should probably use some proper combine
          hasher(headers.getPathValue()));
}

void SimpleCache::init(std::size_t size) {
  if (size == 0) size = 1000003; // config defaults values to 0
  cache_.resize(size);
  for (auto it = cache_.begin(); it != cache_.end(); ++it) *it = new CacheEntry();
}

SimpleCache::~SimpleCache() {
  for (auto it = cache_.begin(); it != cache_.end(); ++it) delete *it;
}

bool SimpleCache::search(RequestHeaderMap& headers, StreamDecoderFilterCallbacks* callbacks, Event::DeferredDeletablePtr& dest, AsyncClient& client) {
  CacheEntry* entry = cache_[getHash(headers) % cache_.size()];
  if (!entry->isCacheable()) return false;
  // just a makeshift way to indicate a HEAD request, would probably get baked in alongside range requests somehow
  bool head_request = headers.getMethodValue() == Http::Headers::get().MethodValues.Head;
  // find the correct entry
  while (true) {
    // most of the time the entry will be populated so check without a mutex first
    if (entry->is_empty_) {
      absl::MutexLock lock(&entry->mtx_);
      if (entry->is_empty_) {
        entry->assign(headers);
        entry->addCallbackToDispatcher(callbacks, head_request, dest);
        break; // go start a new cache specific request without a lock
      } 
    }
    // is_empty_ gets set to false under a lock after host+path gets set
    // at this point we verified under a lock that !is_empty_ so do the
    // possibly "slow" comparison of keys without a lock
    if (entry->isSame(headers)) {
      if (!entry->is_completed_) { // during the herd
        absl::MutexLock lock(&entry->mtx_);
        // a very questionable method to check if this request is the one generated by this cache
        if (entry->stream_ && &entry->stream_->streamInfo() == &callbacks->streamInfo()) {
          entry->stream_ = nullptr; 
          return false; // we want our own request to go through the router normally
        }
        if (!entry->is_completed_) {
          entry->addCallbackToDispatcher(callbacks, head_request, dest);
          return true;
        }
      }
      // don't need to hold the mutex if data is already in the cache
      // possibly would have to hold it once popping from cache is implemented
      dest = std::make_unique<CacheServedRequest>(nullptr, callbacks, head_request);
      auto request = dynamic_cast<CacheServedRequest*>(&*dest);
      entry->reply(callbacks, *request->buffer_, request->start_, head_request);
      return true;
    } 
    if (entry->next_ == nullptr) { // avoid unnecessary locking of unrelated entry
      // could use a separate mutex instead to get rid of the extra condition check
      absl::MutexLock lock(&entry->mtx_);
      if (entry->next_ == nullptr)
        entry->next_ = new CacheEntry();
    }
    entry = entry->next_;
  }
  // at this point we added a new entry so create a new request to fill this entry
  entry->startRequest(headers, client);
  return true;
}

} // namespace Cache
} // namespace Http
} // namespace Envoy