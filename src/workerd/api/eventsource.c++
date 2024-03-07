#include "eventsource.h"
#include "http.h"
#include "streams/common.h"
#include <workerd/jsg/exception.h>
#include <workerd/util/mimetype.h>

namespace workerd::api {

namespace {
class EventSourceSink final: public WritableStreamSink {
public:
  EventSourceSink(EventSource& eventSource) : eventSource(eventSource) {}

  kj::Promise<void> write(const void* buffer, size_t size) override {
    // The event stream is a new-line delimited format where each line represents an event.
    // We need to scan the buffer for end-of-line characters. When we find one, everything before
    // it is pushed into the event queue and we keep scanning. If we do not find an end-of-line
    // sequence in the remaining input, we buffer it and wait for the next write to continue
    // scanning, or until the stream is ended or aborted.

    if (eventSource == kj::none) {
      // Write was received after end() or abort() was called.
      // We'll just ignore the write.
      return kj::READY_NOW;
    }

    kj::ArrayPtr<const char> input(static_cast<const char*>(buffer), size);
    while (input != nullptr) {
      KJ_IF_SOME(found, findEndOfLine(input)) {
          auto prefix = kept.releaseAsArray();
          // Feed the line into the processor.
          feed(kj::str(prefix, input.slice(0, found.pos)));
          input = found.remaining;
          // If we've reached the end of the input, input will == nullptr here.
      } else {
        // No end-of-line found, buffer the input.
        kept.addAll(input.begin(), input.end());
        input = nullptr;
      }
    }

    // Release any buffered events to the EventSource
    release();

    return kj::READY_NOW;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const kj::byte>> pieces) override {
    for (auto& piece : pieces) {
      co_await write(piece.begin(), piece.size());
    }
    co_return;
  }

  kj::Promise<void> end() override {
    // The stream has finished. There's really nothing left to do here. Any partially
    // filled data will be dropped on the floor.
    clear();
    return kj::READY_NOW;
  }

  void abort(kj::Exception reason) override {
    // There's really nothing to do here.
    clear();
  }

private:
  // Retained bytes to be processed in the next write.
  kj::Maybe<EventSource&> eventSource;
  kj::Vector<char> kept;
  kj::Vector<EventSource::PendingMessage> pendingMessages;
  kj::Maybe<EventSource::PendingMessage> currentPendingMessage;

  EventSource::PendingMessage& getPendingMessage() {
    KJ_IF_SOME(pending, currentPendingMessage) {
      return pending;
    }
    return currentPendingMessage.emplace();
  }

  void feed(kj::String line) {
    // Parse line according to the event stream format and dispatch the event.

    // stream        = [ bom ] *event
    // event         = *( comment / field ) end-of-line
    // comment       = colon *any-char end-of-line
    // field         = 1*name-char [ colon [ space ] *any-char ] end-of-line
    // end-of-line   = ( cr lf / cr / lf )

    // ; characters
    // lf            = %x000A ; U+000A LINE FEED (LF)
    // cr            = %x000D ; U+000D CARRIAGE RETURN (CR)
    // space         = %x0020 ; U+0020 SPACE
    // colon         = %x003A ; U+003A COLON (:)
    // bom           = %xFEFF ; U+FEFF BYTE ORDER MARK
    // name-char     = %x0000-0009 / %x000B-000C / %x000E-0039 / %x003B-10FFFF
    //                 ; a scalar value other than U+000A LINE FEED (LF), U+000D CARRIAGE RETURN
    //                   (CR), or U+003A COLON (:)
    // any-char      = %x0000-0009 / %x000B-000C / %x000E-10FFFF
    //                 ; a scalar value other than U+000A LINE FEED (LF) or U+000D CARRIAGE
    //                   RETURN (CR)

    if (line.size() == 0) {
      // Dispatch the current pending message and clear it. If there is no
      // pending message, we'll just ignore the line.
      KJ_IF_SOME(pending, currentPendingMessage) {
        // This message is done and ready to be dispatched. Add it to the
        // pendingMessages list. The next time release() is called, it will
        // be passed off to the EventSource.
        pending.id = KJ_ASSERT_NONNULL(eventSource).getLastEventId().map([](auto& id) {
          return kj::str(id);
        });
        pendingMessages.add(kj::mv(pending));
        currentPendingMessage = kj::none;
      }
    } else if (line[0] == ':') {
      // Ignore the line.
    } else {
      static constexpr auto handle =
          [](auto& self, kj::ArrayPtr<const char> field, kj::ArrayPtr<const char> value) {
        auto& pending = self.getPendingMessage();
        auto& ev = KJ_ASSERT_NONNULL(self.eventSource);
        // Per the spec, only one space after the colon is optional and trimmed.
        // Any other whitespace, or additional spaces aren't accounted for so would
        // be part of the value.
        if (value.size() > 0 && value[0] == ' ') {
          value = value.slice(1);
        }
        if (field == "data"_kjc) {
          pending.data.add(kj::str(value));
        } else if (field == "event"_kjc) {
          pending.event = kj::str(value);
        } else if (field == "id"_kjc) {
          ev.setLastEventId(kj::str(value));
        } else if (field == "retry"_kjc) {
          KJ_IF_SOME(time, kj::str(value).tryParseAs<uint32_t>()) {
            KJ_ASSERT_NONNULL(self.eventSource).setReconnectionTime(time);
          }
          // Ignore the line if it cannot be successfully parsed as a uint32_t
        }
      };

      KJ_IF_SOME(pos, line.findFirst(':')) {
        handle(*this, line.slice(0, pos), line.slice(pos + 1));
      } else {
        handle(*this, line, ""_kjc);
      }
    }
  }

  void release() {
    if (pendingMessages.size() == 0) return;
    auto pending = pendingMessages.releaseAsArray();
    // If the event source is gone, just drop the messages on the floor.
    KJ_IF_SOME(es, eventSource) {
      es.enqueueMessages(kj::mv(pending));
    }
  }

  void clear() {
    eventSource = kj::none;
    kept.clear();
    pendingMessages.clear();
  }

  struct EndOfLine {
    size_t pos;
    kj::ArrayPtr<const char> remaining;
  };
  kj::Maybe<EndOfLine> findEndOfLine(kj::ArrayPtr<const char> input) {
    // The end-of-line marker is either \n, \r, or \r\n
    size_t pos = 0;
    while (pos < input.size()) {
      if (input[pos] == '\n') {
        return EndOfLine{pos, input.slice(pos + 1)};
      } else if (input[pos] == '\r') {
        if (pos + 1 < input.size() && input[pos + 1] == '\n') {
          return EndOfLine{pos, input.slice(pos + 2)};
        }
        return EndOfLine{pos, input.slice(pos + 1)};
      }
      pos++;
    }
    return kj::none;
  }
};

kj::Promise<void> processBody(IoContext& context, kj::Promise<DeferredProxy<void>> promise) {
  try {
    co_await context.waitForDeferredProxy(kj::mv(promise));
  } catch (...) {
    auto ex = kj::getCaughtExceptionAsKj();
    // We would see a disconnection exception if the eventstream is closed for
    // multiple kinds of reasons. If it's a network error and we know we can
    // reconnect, we should try to reconnect.
    if (ex.getType() == kj::Exception::Type::DISCONNECTED) {
      co_return;
    }
    // Propagate the exception up.
    throw;
  }
}
}  // namespace

jsg::Ref<EventSource> EventSource::constructor(
    jsg::Lock& js,
    kj::String url,
    jsg::Optional<EventSourceInit> init) {

  JSG_REQUIRE(IoContext::hasCurrent(), DOMNotSupportedError,
      "An EventSource can only be created within the context of a worker request.");

  KJ_IF_SOME(i, init) {
    KJ_IF_SOME(withCredentials, i.withCredentials) {
      JSG_REQUIRE(!withCredentials, DOMNotSupportedError,
          "The init.withCredentials option is not supported. It must be false or undefined.");
    }
  }

  auto eventsource =jsg::alloc<EventSource>(js,
      JSG_REQUIRE_NONNULL(jsg::Url::tryParse(url.asPtr()),
          DOMSyntaxError,
          kj::str("Cannot open an EventSource to '", url ,"'. The URL is invalid.")),
      kj::mv(init));
  eventsource->start(js);
  return kj::mv(eventsource);
}

EventSource::EventSource(jsg::Lock& js, jsg::Url url, kj::Maybe<EventSourceInit> init)
    : context(IoContext::current()),
      url(kj::mv(url)),
      options(kj::mv(init).orDefault({})),
      abortController(jsg::alloc<AbortController>()),
      readyState(State::CONNECTING) {}

void EventSource::notifyError(jsg::Lock& js, const jsg::JsValue& error, bool reconnecting) {
  if (readyState == State::CLOSED) return;

  // Abort the connection if it hasn't already been. This will be a non-op if the
  // controller has already been aborted.
  abortController->abort(js, error);

  if (!reconnecting) readyState = State::CLOSED;

  // Dispatch the error event.
  dispatchEventImpl(js, jsg::alloc<ErrorEvent>(js, error));

  // Log the error as an uncaught exception for debugging purposes.
  IoContext::current().logUncaughtException(UncaughtExceptionSource::ASYNC_TASK, error);
}

void EventSource::notifyOpen(jsg::Lock& js) {
  if (readyState == State::CLOSED) return;
  readyState = State::OPEN;
  dispatchEventImpl(js, jsg::alloc<OpenEvent>());
}

void EventSource::notifyMessages(jsg::Lock& js, kj::Array<PendingMessage> messages) {
  if (readyState == State::CLOSED) return;
  js.tryCatch([&] {
    for (auto& message : messages) {
        dispatchEventImpl(js, jsg::alloc<MessageEvent>(
            message.event,
            kj::str(kj::delimited(kj::mv(message.data), "\n"_kjc)),
            kj::mv(message.id),
            url));
    }
  }, [&](jsg::Value exception) {
    // If we end up with an exception being thrown in one of the event handlers, we will
    // stop trying to process the messages and instead just error the EventSource.
    notifyError(js, jsg::JsValue(exception.getHandle(js)));
  });
}

void EventSource::reconnect(jsg::Lock& js) {
  readyState = State::CONNECTING;
  abortController = jsg::alloc<AbortController>();
  auto signal = abortController->getSignal();
  context.awaitIo(js, signal->wrap(context.afterLimitTimeout(reconnectionTime)))
      .then(js, JSG_VISITABLE_LAMBDA((self=JSG_THIS), (self), (jsg::Lock& js) mutable {
    self->start(js);
  }), JSG_VISITABLE_LAMBDA((self=JSG_THIS),(self),(jsg::Lock& js, jsg::Value exception) {
    // In this case, it is most likely the EventSource was closed by the user or
    // there was some other failure. We should not continue trying to reconnect.
    self->notifyError(js, jsg::JsValue(exception.getHandle(js)));
  }));
}

void EventSource::start(jsg::Lock& js) {
  if (readyState == State::CLOSED) return;
  auto onSuccess = JSG_VISITABLE_LAMBDA(
      (self=JSG_THIS),
      (self),
      (jsg::Lock& js, jsg::Ref<Response> response) {
    if (self->readyState == State::CLOSED) return js.resolvedPromise();
    IoContext& ioContext = IoContext::current();
    if (!response->getOk()) {
      // Response status code is not 2xx, so we fail.
      // No reconnection attempt should be made.
      auto message = kj::str("The response status code was ", response->getStatus());
      self->notifyError(js,
          jsg::JsValue(jsg::makeDOMException(js.v8Isolate, js.str(message), "AbortError"_kj)));
      return js.resolvedPromise();
    }

    // TODO(cleanup): Using jsg::ByteString here is really annoying. It would be nice to have
    // an internal alternative that doesn't require an allocation.
    KJ_IF_SOME(contentType, response->getHeaders(js)->get(
        jsg::ByteString(kj::str("content-type")))) {
      bool invalid = false;
      KJ_IF_SOME(parsed, MimeType::tryParse(contentType)) {
        invalid = parsed != MimeType::EVENT_STREAM;
      } else {
        invalid = true;
      }
      if (invalid) {
        // No reconnection attempt should be made.
        auto message = kj::str("The content type '", contentType, "' is invalid.");
        self->notifyError(js,
            jsg::JsValue(jsg::makeDOMException(js.v8Isolate,
                js.str(message), "AbortError"_kj)));
        return js.resolvedPromise();
      }
    } else {
      // No reconnection attempt should be made.
      auto message = kj::str("No content type header was present in the response.");
      self->notifyError(js,
          jsg::JsValue(jsg::makeDOMException(js.v8Isolate,
              js.str(message), "AbortError"_kj)));
      return js.resolvedPromise();
    }

    // If the request was redirected, update the URL to the new location.
    if (response->getRedirected()) {
      KJ_IF_SOME(newUrl, jsg::Url::tryParse(response->getUrl())) {
        self->url = kj::mv(newUrl);
      } else {}  // Extra else block to squash compiler warning
    }

    KJ_IF_SOME(body, response->getBody()) {
      self->notifyOpen(js);

      auto onSuccess = JSG_VISITABLE_LAMBDA(
          (self=self.addRef()),
          (self),
          (jsg::Lock& js) {
        // The pump finished. Did the server disconnect? If so, try reconnecting!
        self->notifyError(js, js.error("The server disconnected."), true /* reconnecting */);
        self->reconnect(js);
      });

      auto onFailed = JSG_VISITABLE_LAMBDA(
          (self=self.addRef()),
          (self),
          (jsg::Lock& js, jsg::Value exception) {
        // If the pump fails, catch the error and convert it into an error event.
        // If we got here, it likely isn't just a DISCONNECT event. Let's not
        // try to reconnect at this point.
        self->notifyError(js, jsg::JsValue(exception.getHandle(js)));
      });

      // Well, ok! We're ready to start trying to process the stream! We do so by
      // pumping the body into an EventSourceSink until the body is closed, canceled,
      // or errored.
      return ioContext.awaitIo(js,
          processBody(ioContext, body->pumpTo(js, kj::heap<EventSourceSink>(*self), true)))
              .then(js, kj::mv(onSuccess), kj::mv(onFailed));
    } else {
      // If there is no body, there's nothing to do. We'll treat this as if
      // the server disconnected. If it only happens once, we'll try to reconnect.
      // If it happens again, we'll fail the connection as it is likely indicative
      // of a bug in the server or along the path to the server.
      if (self->previousNoBody) {
         self->notifyError(js, js.error("The server provided no content."));
      } else {
        self->previousNoBody = true;
        self->notifyError(js,
            js.error("The server provided no content. Will try reconnecting"),
            true /* reconnecting */);
        self->reconnect(js);
      }
      return js.resolvedPromise();
    }
  });

  auto onFailed = JSG_VISITABLE_LAMBDA(
      (self=JSG_THIS),
      (self),
      (jsg::Lock& js, jsg::Value exception) {
    self->notifyError(js, jsg::JsValue(exception.getHandle(js)));
    return js.resolvedPromise();
  });

  auto fetcher = options.fetcher.map([](jsg::Ref<Fetcher>& f) { return f.addRef(); });

  fetchImpl(js, kj::mv(fetcher), kj::str(url), RequestInitializerDict {
    .signal = abortController->getSignal(),
  }).then(js, kj::mv(onSuccess), kj::mv(onFailed));
}

void EventSource::close(jsg::Lock& js) {
  if (closeCalled) return;
  closeCalled = true;
  abortController->abort(js, kj::none);
  readyState = State::CLOSED;
}

void EventSource::enqueueMessages(kj::Array<PendingMessage> messages) {
  context.addTask(context.run([this, messages=kj::mv(messages)](auto& lock) mutable {
    notifyMessages(lock, kj::mv(messages));
  }));
}

void EventSource::setReconnectionTime(uint32_t time) {
  // We enforce both a min and max reconnection time. The minimum is 1 second,
  // and the maximum is 10 seconds.
  reconnectionTime =
      kj::max(kj::min(time, MAX_RECONNECTION_TIME), MIN_RECONNECTION_TIME) * kj::MILLISECONDS;
}

kj::Maybe<kj::StringPtr> EventSource::getLastEventId() {
  return lastEventId.map([](kj::String& s) { return s.asPtr(); });
}

void EventSource::setLastEventId(kj::String id) {
  lastEventId = kj::mv(id);
}

void EventSource::visitForGc(jsg::GcVisitor& visitor) {
  visitor.visit(options.fetcher, abortController);
}

void EventSource::visitForMemoryInfo(jsg::MemoryTracker& tracker) const {
  tracker.trackField("fetcher", options.fetcher);
  tracker.trackField("abortController", abortController);
  tracker.trackField("url", url);
  tracker.trackField("lastEventId", lastEventId);
}

}  // namespace workerd::api
