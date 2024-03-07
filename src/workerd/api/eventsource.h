#pragma once
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/url.h>
#include "basics.h"

namespace workerd::api {

using kj::uint;
class Fetcher;

// Implements the web standard EventSource API.
class EventSource: public EventTarget {
public:
  class ErrorEvent final: public Event {
  public:
    ErrorEvent(jsg::Lock& js, const jsg::JsValue& error)
        : Event(kj::str("error")),
          error(js, error) {}

    static jsg::Ref<ErrorEvent> constructor() = delete;
    JSG_RESOURCE_TYPE(ErrorEvent) {
      JSG_INHERIT(Event);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(error, getError);
    }

  private:
    jsg::JsRef<jsg::JsValue> error;

    jsg::JsValue getError(jsg::Lock& js) { return error.getHandle(js); }
  };

  class OpenEvent final: public Event {
  public:
    OpenEvent() : Event(kj::str("open")) {}
    static jsg::Ref<ErrorEvent> constructor() = delete;
    JSG_RESOURCE_TYPE(OpenEvent) {
      JSG_INHERIT(Event);
    }
  };

  class MessageEvent final: public Event {
  public:
    explicit MessageEvent(kj::Maybe<kj::String>& type,
                 kj::String data,
                 kj::Maybe<kj::String> lastEventId,
                 jsg::Url& url)
        : Event(kj::mv(type).orDefault([] { return kj::str("message");})),
          data(kj::mv(data)),
          lastEventId(kj::mv(lastEventId)),
          origin(url.getOrigin()) {}

    static jsg::Ref<ErrorEvent> constructor() = delete;
    JSG_RESOURCE_TYPE(MessageEvent) {
      JSG_INHERIT(Event);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(data, getData);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(origin, getOrigin);
      JSG_LAZY_READONLY_INSTANCE_PROPERTY(lastEventId, getLastEventId);
    }

  private:
    kj::String data;
    kj::Maybe<kj::String> lastEventId;
    kj::Array<const char> origin;

    kj::StringPtr getData() { return data; }
    kj::Maybe<kj::StringPtr> getLastEventId() {
      return lastEventId.map([](kj::String& s) { return s.asPtr(); });
    }
    kj::ArrayPtr<const char> getOrigin() { return origin; }
  };

  struct EventSourceInit {
    // We don't actually make use of the standard withCredentials option. If this is set to
    // any truthy value, we'll throw.
    jsg::Optional<bool> withCredentials;

    // This is a non-standard workers-specific extension that allows the EventSource to
    // use a custom Fetcher instance.
    jsg::Optional<jsg::Ref<Fetcher>> fetcher;
    JSG_STRUCT(withCredentials, fetcher);
  };

  enum class State {
    CONNECTING = 0,
    OPEN = 1,
    CLOSED = 2,
  };

  EventSource(jsg::Lock& js, jsg::Url url, kj::Maybe<EventSourceInit> init = kj::none);

  static jsg::Ref<EventSource> constructor(jsg::Lock& js,
                                           kj::String url,
                                           jsg::Optional<EventSourceInit> init);

  kj::ArrayPtr<const char> getUrl() const { return url.getHref(); }
  bool getWithCredentials() const { return options.withCredentials.orDefault(false); }
  uint getReadyState() const { return static_cast<uint>(readyState); }

  void close(jsg::Lock& js);

  JSG_RESOURCE_TYPE(EventSource) {
    JSG_METHOD(close);
    JSG_READONLY_PROTOTYPE_PROPERTY(url, getUrl);
    JSG_READONLY_PROTOTYPE_PROPERTY(withCredentials, getWithCredentials);
    JSG_READONLY_PROTOTYPE_PROPERTY(readyState, getReadyState);
    JSG_STATIC_CONSTANT_NAMED(CONNECTING, static_cast<uint>(State::CONNECTING));
    JSG_STATIC_CONSTANT_NAMED(OPEN, static_cast<uint>(State::OPEN));
    JSG_STATIC_CONSTANT_NAMED(CLOSED, static_cast<uint>(State::CLOSED));
  }

  struct PendingMessage {
    kj::Vector<kj::String> data;
    kj::Maybe<kj::String> event;
    kj::Maybe<kj::String> id;
  };

  // Called by the internal implementation to notify the EventSource about messages
  // received from the server.
  void enqueueMessages(kj::Array<PendingMessage> messages);

  // Called by the internal implementation to notify the EventSource that the server
  // has provided a new reconnection time.
  void setReconnectionTime(uint32_t time);

  // Called by the internal implementation to retrieve the last event id that was
  // specified by the server.
  kj::Maybe<kj::StringPtr> getLastEventId();

  // Called by the internal implementation to set the last event id that was specified
  // by the server.
  void setLastEventId(kj::String id);

  void visitForGc(jsg::GcVisitor& visitor);
  void visitForMemoryInfo(jsg::MemoryTracker& tracker) const;

private:
  IoContext& context;
  jsg::Url url;
  EventSourceInit options;
  jsg::Ref<AbortController> abortController;
  State readyState;
  kj::Maybe<kj::String> lastEventId;

  // Indicates that the close method has been previously called.
  bool closeCalled = false;

  // Indicates that the server previous responded with no content after a
  // successful connection. This is likely indicative of a bug on the server.
  // If this happens once, we'll try to reconnect. If it happens again, we'll
  // fail the connection.
  bool previousNoBody = false;

  // The default reconnection wait time. This is fairly arbitrary and is left
  // entirely up to the implementation. The event stream can provide a new value
  static constexpr auto DEFAULT_RECONNECTION_TIME = 2 * kj::SECONDS;
  static constexpr uint32_t MIN_RECONNECTION_TIME = 1000;
  static constexpr uint32_t MAX_RECONNECTION_TIME = 10 * 1000;

  kj::Duration reconnectionTime = DEFAULT_RECONNECTION_TIME;

  void notifyOpen(jsg::Lock& js);
  void notifyError(jsg::Lock& js, const jsg::JsValue& error, bool reconnecting = false);
  void notifyMessages(jsg::Lock& js, kj::Array<PendingMessage> messages);

  void start(jsg::Lock& js);
  void reconnect(jsg::Lock& js);
};

}  // namespace workerd::api

#define EW_EVENTSOURCE_ISOLATE_TYPES      \
  api::EventSource,                       \
  api::EventSource::ErrorEvent,           \
  api::EventSource::OpenEvent,            \
  api::EventSource::MessageEvent,         \
  api::EventSource::EventSourceInit
