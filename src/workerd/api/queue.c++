// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "queue.h"

#include "util.h"

#include <workerd/api/global-scope.h>
#include <workerd/io/features.h>
#include <workerd/io/tracer.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/ser.h>
#include <workerd/util/mimetype.h>
#include <workerd/util/strings.h>

#include <kj/encoding.h>

namespace workerd::api {

namespace {

// Header for the message format.
static constexpr kj::StringPtr HDR_MSG_FORMAT = "X-Msg-Fmt"_kj;

// Header for the message delivery delay.
static constexpr kj::StringPtr HDR_MSG_DELAY = "X-Msg-Delay-Secs"_kj;

kj::StringPtr validateContentType(kj::StringPtr contentType) {
  auto lowerCase = toLower(contentType);
  if (lowerCase == IncomingQueueMessage::ContentType::TEXT) {
    return IncomingQueueMessage::ContentType::TEXT;
  } else if (lowerCase == IncomingQueueMessage::ContentType::BYTES) {
    return IncomingQueueMessage::ContentType::BYTES;
  } else if (lowerCase == IncomingQueueMessage::ContentType::JSON) {
    return IncomingQueueMessage::ContentType::JSON;
  } else if (lowerCase == IncomingQueueMessage::ContentType::V8) {
    return IncomingQueueMessage::ContentType::V8;
  } else {
    JSG_FAIL_REQUIRE(TypeError, kj::str("Unsupported queue message content type: ", contentType));
  }
}

struct Serialized {
  kj::Maybe<kj::OneOf<kj::String, kj::Array<kj::byte>, jsg::BufferSource, jsg::BackingStore>> own;
  // Holds onto the owner of a given array of serialized data.
  kj::ArrayPtr<kj::byte> data;
  // A pointer into that data that can be directly written into an outgoing queue send, regardless
  // of its holder.
};

Serialized serializeV8(jsg::Lock& js, const jsg::JsValue& body) {
  // Use a specific serialization version to avoid sending messages using a new version before all
  // runtimes at the edge know how to read it.
  jsg::Serializer serializer(js,
      jsg::Serializer::Options{
        .version = 15,
        .omitHeader = false,
      });
  serializer.write(js, jsg::JsValue(body));
  kj::Array<kj::byte> bytes = serializer.release().data;
  Serialized result;
  result.data = bytes;
  result.own = kj::mv(bytes);
  return kj::mv(result);
}

// Control whether the serialize() method makes a deep copy of provided ArrayBuffer types or if it
// just returns a shallow reference that is only valid until the given method returns.
enum class SerializeArrayBufferBehavior {
  DEEP_COPY,
  SHALLOW_REFERENCE,
};

Serialized serialize(jsg::Lock& js,
    const jsg::JsValue& body,
    kj::StringPtr contentType,
    SerializeArrayBufferBehavior bufferBehavior) {
  if (contentType == IncomingQueueMessage::ContentType::TEXT) {
    JSG_REQUIRE(body.isString(), TypeError,
        kj::str("Content Type \"", IncomingQueueMessage::ContentType::TEXT,
            "\" requires a value of type string, but received: ", body.typeOf(js)));

    kj::String s = body.toString(js);
    Serialized result;
    result.data = s.asBytes();
    result.own = kj::mv(s);
    return kj::mv(result);
  } else if (contentType == IncomingQueueMessage::ContentType::BYTES) {
    JSG_REQUIRE(body.isArrayBufferView(), TypeError,
        kj::str("Content Type \"", IncomingQueueMessage::ContentType::BYTES,
            "\" requires a value of type ArrayBufferView, but received: ", body.typeOf(js)));

    jsg::BufferSource source(js, body);
    if (bufferBehavior == SerializeArrayBufferBehavior::SHALLOW_REFERENCE) {
      // If we know the data will be consumed synchronously, we can avoid copying it.
      Serialized result;
      result.data = source.asArrayPtr();
      result.own = kj::mv(source);
      return kj::mv(result);
    } else if (source.canDetach(js)) {
      // Prefer detaching the input ArrayBuffer whenever possible to avoid needing to copy it.
      auto backingSource = source.detach(js);
      Serialized result;
      result.data = backingSource.asArrayPtr();
      result.own = kj::mv(backingSource);
      return kj::mv(result);
    } else {
      kj::Array<kj::byte> bytes = kj::heapArray(source.asArrayPtr());
      Serialized result;
      result.data = bytes;
      result.own = kj::mv(bytes);
      return kj::mv(result);
    }
  } else if (contentType == IncomingQueueMessage::ContentType::JSON) {
    kj::String s = body.toJson(js);
    Serialized result;
    result.data = s.asBytes();
    result.own = kj::mv(s);
    return kj::mv(result);
  } else if (contentType == IncomingQueueMessage::ContentType::V8) {
    return serializeV8(js, body);
  } else {
    JSG_FAIL_REQUIRE(TypeError, kj::str("Unsupported queue message content type: ", contentType));
  }
}

struct SerializedWithOptions {
  Serialized body;
  kj::Maybe<kj::StringPtr> contentType;
  kj::Maybe<int> delaySeconds;
};

jsg::JsValue deserialize(
    jsg::Lock& js, kj::Array<kj::byte> body, kj::Maybe<kj::StringPtr> contentType) {
  auto type = contentType.orDefault(IncomingQueueMessage::ContentType::V8);

  if (type == IncomingQueueMessage::ContentType::TEXT) {
    return js.str(body);
  } else if (type == IncomingQueueMessage::ContentType::BYTES) {
    return jsg::JsValue(js.bytes(kj::mv(body)).getHandle(js));
  } else if (type == IncomingQueueMessage::ContentType::JSON) {
    return jsg::JsValue::fromJson(js, body.asChars());
  } else if (type == IncomingQueueMessage::ContentType::V8) {
    return jsg::JsValue(jsg::Deserializer(js, body.asPtr()).readValue(js));
  } else {
    JSG_FAIL_REQUIRE(TypeError, kj::str("Unsupported queue message content type: ", type));
  }
}

jsg::JsValue deserialize(jsg::Lock& js, rpc::QueueMessage::Reader message) {
  kj::StringPtr type = message.getContentType();
  if (type == "") {
    // default to v8 format
    type = IncomingQueueMessage::ContentType::V8;
  }

  if (type == IncomingQueueMessage::ContentType::TEXT) {
    return js.str(message.getData().asChars());
  } else if (type == IncomingQueueMessage::ContentType::BYTES) {
    kj::Array<kj::byte> bytes = kj::heapArray(message.getData().asBytes());
    return jsg::JsValue(js.bytes(kj::mv(bytes)).getHandle(js));
  } else if (type == IncomingQueueMessage::ContentType::JSON) {
    return jsg::JsValue::fromJson(js, message.getData().asChars());
  } else if (type == IncomingQueueMessage::ContentType::V8) {
    return jsg::JsValue(jsg::Deserializer(js, message.getData()).readValue(js));
  } else {
    JSG_FAIL_REQUIRE(TypeError, kj::str("Unsupported queue message content type: ", type));
  }
}
}  // namespace

kj::Promise<void> WorkerQueue::send(
    jsg::Lock& js, jsg::JsValue body, jsg::Optional<SendOptions> options) {
  auto& context = IoContext::current();

  JSG_REQUIRE(!body.isUndefined(), TypeError, "Message body cannot be undefined");

  auto headers = kj::HttpHeaders(context.getHeaderTable());
  headers.set(kj::HttpHeaderId::CONTENT_TYPE, MimeType::OCTET_STREAM.toString());

  kj::Maybe<kj::StringPtr> contentType;
  KJ_IF_SOME(opts, options) {
    KJ_IF_SOME(type, opts.contentType) {
      auto validatedType = validateContentType(type);
      headers.add(HDR_MSG_FORMAT, validatedType);
      contentType = validatedType;
    }
    KJ_IF_SOME(secs, opts.delaySeconds) {
      headers.add(HDR_MSG_DELAY, kj::str(secs));
    }
  }

  Serialized serialized;
  KJ_IF_SOME(type, contentType) {
    serialized = serialize(js, body, type, SerializeArrayBufferBehavior::DEEP_COPY);
  } else if (workerd::FeatureFlags::get(js).getQueuesJsonMessages()) {
    headers.add("X-Msg-Fmt", IncomingQueueMessage::ContentType::JSON);
    serialized = serialize(
        js, body, IncomingQueueMessage::ContentType::JSON, SerializeArrayBufferBehavior::DEEP_COPY);
  } else {
    // TODO(cleanup) send message format header (v8) by default
    serialized = serializeV8(js, body);
  }

  // The stage that we're sending a subrequest to provides a base URL that includes a scheme, the
  // queue broker's domain, and the start of the URL path including the account ID and queue ID. All
  // we have to do is provide the end of the path (which is "/message") to send a single message.

  auto client = context.getHttpClient(subrequestChannel, true, kj::none, "queue_send"_kjc);
  auto req = client->request(
      kj::HttpMethod::POST, "https://fake-host/message"_kjc, headers, serialized.data.size());

  static constexpr auto handleSend = [](auto req, auto serialized,
                                         auto client) -> kj::Promise<void> {
    co_await req.body->write(serialized.data);
    auto response = co_await req.response;

    JSG_REQUIRE(
        response.statusCode == 200, Error, kj::str("Queue send failed: ", response.statusText));

    // Read and discard response body, otherwise we might burn the HTTP connection.
    co_await response.body->readAllBytes().ignoreResult();
  };

  return handleSend(kj::mv(req), kj::mv(serialized), kj::mv(client))
      .attach(context.registerPendingEvent());
};

kj::Promise<void> WorkerQueue::sendBatch(jsg::Lock& js,
    jsg::Sequence<MessageSendRequest> batch,
    jsg::Optional<SendBatchOptions> options) {
  auto& context = IoContext::current();

  JSG_REQUIRE(batch.size() > 0, TypeError, "sendBatch() requires at least one message");

  size_t totalSize = 0;
  size_t largestMessage = 0;
  auto messageCount = batch.size();
  auto builder = kj::heapArrayBuilder<SerializedWithOptions>(messageCount);
  for (auto& message: batch) {
    auto body = message.body.getHandle(js);
    JSG_REQUIRE(!body.isUndefined(), TypeError, "Message body cannot be undefined");

    SerializedWithOptions item;
    KJ_IF_SOME(secs, message.delaySeconds) {
      item.delaySeconds = secs;
    }

    KJ_IF_SOME(contentType, message.contentType) {
      item.contentType = validateContentType(contentType);
      item.body = serialize(js, body, contentType, SerializeArrayBufferBehavior::SHALLOW_REFERENCE);
    } else if (workerd::FeatureFlags::get(js).getQueuesJsonMessages()) {
      item.contentType = IncomingQueueMessage::ContentType::JSON;
      item.body = serialize(js, body, IncomingQueueMessage::ContentType::JSON,
          SerializeArrayBufferBehavior::SHALLOW_REFERENCE);
    } else {
      item.body = serializeV8(js, body);
    }

    builder.add(kj::mv(item));
    totalSize += builder.back().body.data.size();
    largestMessage = kj::max(largestMessage, builder.back().body.data.size());
  }
  auto serializedBodies = builder.finish();

  // Construct the request body by concatenating the messages together into a JSON message.
  // Done manually to minimize copies, although it'd be nice to make this safer.
  // (totalSize + 2) / 3 * 4 is equivalent to ceil(totalSize / 3) * 4 for base64 encoding overhead.
  auto estimatedSize = (totalSize + 2) / 3 * 4 + messageCount * 64 + 32;
  kj::Vector<char> bodyBuilder(estimatedSize);
  bodyBuilder.addAll("{\"messages\":["_kj);
  for (size_t i = 0; i < messageCount; ++i) {
    bodyBuilder.addAll("{\"body\":\""_kj);
    // TODO(perf): We should be able to encode the data directly into bodyBuilder's buffer to
    // eliminate a lot of data copying (whereas now encodeBase64 allocates a new buffer of its own
    // to hold its result, which we then have to copy into bodyBuilder).
    bodyBuilder.addAll(kj::encodeBase64(serializedBodies[i].body.data));
    bodyBuilder.add('"');

    KJ_IF_SOME(contentType, serializedBodies[i].contentType) {
      bodyBuilder.addAll(",\"contentType\":\""_kj);
      bodyBuilder.addAll(contentType);
      bodyBuilder.add('"');
    }

    KJ_IF_SOME(delaySecs, serializedBodies[i].delaySeconds) {
      bodyBuilder.addAll(",\"delaySecs\": "_kj);
      bodyBuilder.addAll(kj::str(delaySecs));
    }

    bodyBuilder.addAll("}"_kj);
    if (i < messageCount - 1) {
      bodyBuilder.add(',');
    }
  }
  bodyBuilder.addAll("]}"_kj);
  bodyBuilder.add('\0');
  KJ_DASSERT(bodyBuilder.size() <= estimatedSize);
  kj::String body(bodyBuilder.releaseAsArray());
  KJ_DASSERT(jsg::JsValue::fromJson(js, body).isObject());

  auto client = context.getHttpClient(subrequestChannel, true, kj::none, "queue_send"_kjc);

  // We add info about the size of the batch to the headers so that the queue implementation can
  // decide whether it's too large.
  // TODO(someday): Enforce the size limits here instead for very slightly better performance.
  auto headers = kj::HttpHeaders(context.getHeaderTable());
  headers.add("CF-Queue-Batch-Count"_kj, kj::str(messageCount));
  headers.add("CF-Queue-Batch-Bytes"_kj, kj::str(totalSize));
  headers.add("CF-Queue-Largest-Msg"_kj, kj::str(largestMessage));
  headers.set(kj::HttpHeaderId::CONTENT_TYPE, MimeType::JSON.toString());

  KJ_IF_SOME(opts, options) {
    KJ_IF_SOME(secs, opts.delaySeconds) {
      headers.add(HDR_MSG_DELAY, kj::str(secs));
    }
  }

  // The stage that we're sending a subrequest to provides a base URL that includes a scheme, the
  // queue broker's domain, and the start of the URL path including the account ID and queue ID. All
  // we have to do is provide the end of the path (which is "/batch") to send a message batch.

  auto req =
      client->request(kj::HttpMethod::POST, "https://fake-host/batch"_kjc, headers, body.size());

  static constexpr auto handleWrite = [](auto req, auto body, auto client) -> kj::Promise<void> {
    co_await req.body->write(body.asBytes());
    auto response = co_await req.response;

    JSG_REQUIRE(response.statusCode == 200, Error,
        kj::str("Queue sendBatch failed: ", response.statusText));

    // Read and discard response body, otherwise we might burn the HTTP connection.
    co_await response.body->readAllBytes().ignoreResult();
  };

  return handleWrite(kj::mv(req), kj::mv(body), kj::mv(client))
      .attach(context.registerPendingEvent());
};

QueueMessage::QueueMessage(
    jsg::Lock& js, rpc::QueueMessage::Reader message, IoPtr<QueueEventResult> result)
    : id(kj::str(message.getId())),
      timestamp(message.getTimestampNs() * kj::NANOSECONDS + kj::UNIX_EPOCH),
      body(deserialize(js, message).addRef(js)),
      attempts(message.getAttempts()),
      result(result) {}
// Note that we must make deep copies of all data here since the incoming Reader may be
// deallocated while JS's GC wrappers still exist.

QueueMessage::QueueMessage(
    jsg::Lock& js, IncomingQueueMessage message, IoPtr<QueueEventResult> result)
    : id(kj::mv(message.id)),
      timestamp(message.timestamp),
      body(deserialize(js, kj::mv(message.body), message.contentType).addRef(js)),
      attempts(message.attempts),
      result(result) {}

jsg::JsValue QueueMessage::getBody(jsg::Lock& js) {
  return body.getHandle(js);
}

void QueueMessage::retry(jsg::Optional<QueueRetryOptions> options) {
  if (result->ackAll) {
    auto msg = kj::str("Received a call to retry() on message ", id,
        " after ackAll() was already called. "
        "Calling retry() on a message after calling ackAll() has no effect.");
    IoContext::current().logWarning(msg);
    return;
  }

  if (result->explicitAcks.contains(id)) {
    auto msg = kj::str("Received a call to retry() on message ", id,
        " after ack() was already called. "
        "Calling retry() on a message after calling ack() has no effect.");
    IoContext::current().logWarning(msg);
    return;
  }

  auto& entry = result->retries.upsert(kj::heapString(id), {});
  KJ_IF_SOME(opts, options) {
    KJ_IF_SOME(secs, opts.delaySeconds) {
      entry.value.delaySeconds = secs;
    }
  }
}

void QueueMessage::ack() {
  if (result->ackAll) {
    return;
  }

  if (result->retryBatch.retry) {
    auto msg = kj::str("Received a call to ack() on message ", id,
        " after retryAll() was already called. "
        "Calling ack() on a message after calling retryAll() has no effect.");
    IoContext::current().logWarning(msg);
    return;
  }

  if (result->retries.find(id) != kj::none) {
    auto msg = kj::str("Received a call to ack() on message ", id,
        " after retry() was already called. "
        "Calling ack() on a message after calling retry() has no effect.");
    IoContext::current().logWarning(msg);
    return;
  }
  result->explicitAcks.findOrCreate(id, [this]() { return kj::heapString(id); });
}

QueueEvent::QueueEvent(
    jsg::Lock& js, rpc::EventDispatcher::QueueParams::Reader params, IoPtr<QueueEventResult> result)
    : ExtendableEvent("queue"),
      queueName(kj::heapString(params.getQueueName())),
      result(result) {
  // Note that we must make deep copies of all data here since the incoming Reader may be
  // deallocated while JS's GC wrappers still exist.
  auto incoming = params.getMessages();
  auto messagesBuilder = kj::heapArrayBuilder<jsg::Ref<QueueMessage>>(incoming.size());
  for (auto i: kj::indices(incoming)) {
    messagesBuilder.add(js.alloc<QueueMessage>(js, incoming[i], result));
  }
  messages = messagesBuilder.finish();
}

QueueEvent::QueueEvent(jsg::Lock& js, Params params, IoPtr<QueueEventResult> result)
    : ExtendableEvent("queue"),
      queueName(kj::mv(params.queueName)),
      result(result) {
  auto messagesBuilder = kj::heapArrayBuilder<jsg::Ref<QueueMessage>>(params.messages.size());
  for (auto i: kj::indices(params.messages)) {
    messagesBuilder.add(js.alloc<QueueMessage>(js, kj::mv(params.messages[i]), result));
  }
  messages = messagesBuilder.finish();
}

void QueueEvent::retryAll(jsg::Optional<QueueRetryOptions> options) {
  if (result->ackAll) {
    IoContext::current().logWarning(
        "Received a call to retryAll() after ackAll() was already called. "
        "Calling retryAll() after calling ackAll() has no effect.");
    return;
  }

  result->retryBatch.retry = true;
  KJ_IF_SOME(opts, options) {
    KJ_IF_SOME(secs, opts.delaySeconds) {
      result->retryBatch.delaySeconds = secs;
    }
  }
}

void QueueEvent::ackAll() {
  if (result->retryBatch.retry) {
    IoContext::current().logWarning(
        "Received a call to ackAll() after retryAll() was already called. "
        "Calling ackAll() after calling retryAll() has no effect.");
    return;
  }
  result->ackAll = true;
}

namespace {

struct StartQueueEventResponse {
  jsg::Ref<QueueEvent> event = nullptr;
  kj::Maybe<kj::Promise<void>> exportedHandlerProm;
  bool isServiceWorkerHandler = false;
};

StartQueueEventResponse startQueueEvent(EventTarget& globalEventTarget,
    kj::OneOf<rpc::EventDispatcher::QueueParams::Reader, QueueEvent::Params> params,
    IoPtr<QueueEventResult> result,
    Worker::Lock& lock,
    kj::Maybe<ExportedHandler&> exportedHandler,
    const jsg::TypeHandler<QueueExportedHandler>& handlerHandler) {
  jsg::Lock& js = lock;
  jsg::Ref<QueueEvent> event(nullptr);
  KJ_SWITCH_ONEOF(params) {
    KJ_CASE_ONEOF(p, rpc::EventDispatcher::QueueParams::Reader) {
      event = js.alloc<QueueEvent>(js, p, result);
    }
    KJ_CASE_ONEOF(p, QueueEvent::Params) {
      event = js.alloc<QueueEvent>(js, kj::mv(p), result);
    }
  }

  kj::Maybe<kj::Promise<void>> exportedHandlerProm;
  bool isServiceWorkerHandler = false;
  KJ_IF_SOME(h, exportedHandler) {
    auto queueHandler = KJ_ASSERT_NONNULL(handlerHandler.tryUnwrap(lock, h.self.getHandle(lock)));
    KJ_IF_SOME(f, queueHandler.queue) {
      auto promise = f(lock, js.alloc<QueueController>(event.addRef()),
          jsg::JsValue(h.env.getHandle(js)).addRef(js), h.getCtx())
                         .then([event = event.addRef()]() mutable {
        event->setCompletionStatus(QueueEvent::CompletedSuccessfully{});
      }, [event = event.addRef()](kj::Exception&& e) mutable {
        event->setCompletionStatus(QueueEvent::CompletedWithError{kj::cp(e)});
        return kj::mv(e);
      });
      if (FeatureFlags::get(js).getQueueConsumerNoWaitForWaitUntil()) {
        exportedHandlerProm = kj::mv(promise);
      } else {
        event->waitUntil(kj::mv(promise));
      }
    } else {
      lock.logWarningOnce("Received a QueueEvent but we lack a handler for QueueEvents. "
                          "Did you remember to export a queue() function?");
      JSG_FAIL_REQUIRE(Error, "Handler does not export a queue() function.");
    }
  } else {
    isServiceWorkerHandler = true;
    if (globalEventTarget.getHandlerCount("queue") == 0) {
      lock.logWarningOnce("Received a QueueEvent but we lack an event listener for queue events. "
                          "Did you remember to call addEventListener(\"queue\", ...)?");
      JSG_FAIL_REQUIRE(Error, "No event listener registered for queue messages.");
    }
    globalEventTarget.dispatchEventImpl(lock, event.addRef());
    event->setCompletionStatus(QueueEvent::CompletedSuccessfully{});
  }

  return StartQueueEventResponse{
    kj::mv(event), kj::mv(exportedHandlerProm), isServiceWorkerHandler};
}

}  // namespace

kj::Promise<WorkerInterface::CustomEvent::Result> QueueCustomEventImpl::run(
    kj::Own<IoContext_IncomingRequest> incomingRequest,
    kj::Maybe<kj::StringPtr> entrypointName,
    Frankenvalue props,
    kj::TaskSet& waitUntilTasks) {
  // This method has three main chunks of logic:
  // 1. Do all necessary setup work. This starts right below this comment.
  // 2. Call into the worker's queue event handler.
  // 3. Wait on the necessary portions of the worker's code to complete.
  incomingRequest->delivered();
  auto& context = incomingRequest->getContext();

  kj::String queueName;
  uint32_t batchSize;
  KJ_SWITCH_ONEOF(params) {
    KJ_CASE_ONEOF(p, rpc::EventDispatcher::QueueParams::Reader) {
      queueName = kj::heapString(p.getQueueName());
      batchSize = p.getMessages().size();
    }
    KJ_CASE_ONEOF(p, QueueEvent::Params) {
      queueName = kj::heapString(p.queueName);
      batchSize = p.messages.size();
    }
  }

  KJ_IF_SOME(t, incomingRequest->getWorkerTracer()) {
    t.setEventInfo(context.getInvocationSpanContext(), context.now(),
        tracing::QueueEventInfo(kj::str(queueName), batchSize));
  }

  // Create a custom refcounted type for holding the queueEvent so that we can pass it to the
  // waitUntil'ed callback safely without worrying about whether this coroutine gets canceled.
  struct QueueEventHolder: public kj::Refcounted {
    jsg::Ref<QueueEvent> event = nullptr;
    kj::Maybe<kj::Promise<void>> exportedHandlerProm;
    bool isServiceWorkerHandler = false;
  };
  auto queueEventHolder = kj::refcounted<QueueEventHolder>();

  // 2. This is where we call into the worker's queue event handler
  auto runProm = context.run(
      [this, entrypointName = entrypointName, &context, queueEvent = kj::addRef(*queueEventHolder),
          &metrics = incomingRequest->getMetrics(),
          props = kj::mv(props)](Worker::Lock& lock) mutable {
    jsg::AsyncContextFrame::StorageScope traceScope = context.makeAsyncTraceScope(lock);

    auto& typeHandler = lock.getWorker().getIsolate().getApi().getQueueTypeHandler(lock);
    auto startResp = startQueueEvent(lock.getGlobalScope(), kj::mv(params),
        context.addObject(result), lock,
        lock.getExportedHandler(entrypointName, kj::mv(props), context.getActor()), typeHandler);
    queueEvent->event = kj::mv(startResp.event);
    queueEvent->exportedHandlerProm = kj::mv(startResp.exportedHandlerProm);
    queueEvent->isServiceWorkerHandler = startResp.isServiceWorkerHandler;
  });

  // 3. Now that we've (asynchronously) called into the event handler, wait on all necessary async
  // work to complete. This logic is split into two completely separate code paths depending on
  // whether the queueConsumerNoWaitForWaitUntil compatibility flag is enabled.
  // * In the enabled path, the queue event can be considered complete as soon as the event handler
  //   returns and the promise that it returns (if any) has resolved.
  // * In the disabled path, the queue event isn't complete until all waitUntil'ed promises resolve.
  //   This was how Queues originally worked, but made for a poor user experience.
  auto compatFlags = context.getWorker().getIsolate().getApi().getFeatureFlags();
  if (compatFlags.getQueueConsumerNoWaitForWaitUntil()) {
    // The user has opted in to only waiting on their event handler rather than all waitUntil'd
    // promises.
    auto timeoutPromise = context.getLimitEnforcer().limitScheduled();
    // Start invoking the queue handler. The promise chain here is intended to mimic the behavior of
    // finishScheduled, but only waiting on the promise returned by the event handler rather than on
    // all waitUntil'ed promises.
    auto outcome = co_await runProm
                       .then([queueEvent = kj::addRef(
                                  *queueEventHolder)]() mutable -> kj::Promise<EventOutcome> {
      // If the queue handler returned a promise, wait on the promise.
      KJ_IF_SOME(handlerProm, queueEvent->exportedHandlerProm) {
        return handlerProm.then([]() { return EventOutcome::OK; });
      }
      // If not, we can consider the invocation complete.
      return EventOutcome::OK;
    })
                       .catch_([](kj::Exception&& e) {
      // If any exceptions were thrown, mark the outcome accordingly.
      return EventOutcome::EXCEPTION;
    })
                       .exclusiveJoin(timeoutPromise.then([] {
      // Join everything against a timeout to ensure queue handlers can't run forever.
      return EventOutcome::EXCEEDED_CPU;
    })).exclusiveJoin(context.onAbort().then([] {
      // Also handle anything that might cause the worker to get aborted.
      // This is a change from the outcome we returned on abort before the compat flag, but better
      // matches the behavior of fetch() handlers and the semantics of what's actually happening.
      return EventOutcome::EXCEPTION;
    }, [](kj::Exception&&) { return EventOutcome::EXCEPTION; }));

    if (outcome == EventOutcome::OK && queueEventHolder->isServiceWorkerHandler) {
      // HACK: For service-worker syntax, we effectively ignore the compatibility flag and wait
      // for all waitUntil tasks anyway, since otherwise there's no way to do async work from an
      // event listener callback.
      // It'd be nicer if we could fall through to the code below for the non-compat-flag logic in
      // this case, but we don't even know if the worker uses service worker syntax until after
      // runProm resolves, so we just copy the bare essentials here.
      auto result = co_await incomingRequest->finishScheduled();
      bool completed = result == IoContext_IncomingRequest::FinishScheduledResult::COMPLETED;
      outcome = completed ? context.waitUntilStatus() : EventOutcome::EXCEEDED_CPU;
    } else {
      // We're responsible for calling drain() on the incomingRequest to ensure that waitUntil tasks
      // can continue to run in the backgound for a while even after we return a result to the
      // caller of this event. But this is only needed in this code path because in all other code
      // paths we call incomingRequest->finishScheduled(), which already takes care of waiting on
      // waitUntil tasks.
      waitUntilTasks.add(incomingRequest->drain().attach(
          kj::mv(incomingRequest), kj::addRef(*queueEventHolder), kj::addRef(*this)));
    }

    KJ_IF_SOME(status, context.getLimitEnforcer().getLimitsExceeded()) {
      outcome = status;
    }
    co_return WorkerInterface::CustomEvent::Result{.outcome = outcome};
  } else {
    // The user has not opted in to the new waitUntil behavior, so we need to add the queue()
    // handler's promise to the waitUntil promises and then wait on them all to finish.
    context.addWaitUntil(kj::mv(runProm));

    // We reuse the finishScheduled() method for convenience, since queues use the same wall clock
    // timeout as scheduled workers.
    auto result = co_await incomingRequest->finishScheduled();
    bool completed = result == IoContext_IncomingRequest::FinishScheduledResult::COMPLETED;

    // Log some debug info if the request timed out or was aborted, to aid in debugging situations
    // where consumer workers appear to get stuck and repeatedly take 15 minutes.
    // In particular, detect whether or not the users queue() handler function completed
    // and include info about other waitUntil tasks that may have caused the request to timeout.
    if (!completed) {
      kj::String status;
      if (queueEventHolder->event.get() == nullptr) {
        status = kj::str("Empty");
      } else {
        KJ_SWITCH_ONEOF(queueEventHolder->event->getCompletionStatus()) {
          KJ_CASE_ONEOF(i, QueueEvent::Incomplete) {
            status = kj::str("Incomplete");
            break;
          }
          KJ_CASE_ONEOF(s, QueueEvent::CompletedSuccessfully) {
            status = kj::str("Completed Succesfully");
            break;
          }
          KJ_CASE_ONEOF(e, QueueEvent::CompletedWithError) {
            status = kj::str("Completed with error:", e.error);
            break;
          }
        }
      }
      auto& ioContext = incomingRequest->getContext();
      auto scriptId = ioContext.getWorker().getScript().getId();
      auto tasks = ioContext.getWaitUntilTasks().trace();
      if (result == IoContext_IncomingRequest::FinishScheduledResult::TIMEOUT) {
        KJ_LOG(WARNING, "NOSENTRY queue event hit timeout", scriptId, status, tasks);
      } else if (result == IoContext_IncomingRequest::FinishScheduledResult::ABORTED) {
        // Attempt to grab the error message to understand the reason for the abort.
        // Include a timeout just in case for some unexpected reason the onAbort promise hasn't
        // already rejected.
        kj::String abortError;
        co_await ioContext.onAbort()
            .catch_([&abortError](kj::Exception&& e) {
          abortError = kj::str(e);
        }).exclusiveJoin(ioContext.afterLimitTimeout(1 * kj::MICROSECONDS).then([&abortError]() {
          abortError = kj::str("onAbort() promise has unexpectedly not yet been rejected");
        }));
        KJ_LOG(WARNING, "NOSENTRY queue event aborted", abortError, scriptId, status, tasks);
      }
    }

    co_return WorkerInterface::CustomEvent::Result{
      .outcome = completed ? context.waitUntilStatus() : EventOutcome::EXCEEDED_CPU,
    };
  }
}

kj::Promise<WorkerInterface::CustomEvent::Result> QueueCustomEventImpl::sendRpc(
    capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
    capnp::ByteStreamFactory& byteStreamFactory,
    rpc::EventDispatcher::Client dispatcher) {
  auto req = dispatcher.castAs<rpc::EventDispatcher>().queueRequest();
  KJ_SWITCH_ONEOF(params) {
    KJ_CASE_ONEOF(p, rpc::EventDispatcher::QueueParams::Reader) {
      req.setQueueName(p.getQueueName());
      req.setMessages(p.getMessages());
    }
    KJ_CASE_ONEOF(p, QueueEvent::Params) {
      req.setQueueName(p.queueName);
      auto messages = req.initMessages(p.messages.size());
      for (auto i: kj::indices(p.messages)) {
        messages[i].setId(p.messages[i].id);
        messages[i].setTimestampNs((p.messages[i].timestamp - kj::UNIX_EPOCH) / kj::NANOSECONDS);
        messages[i].setData(p.messages[i].body);
        KJ_IF_SOME(contentType, p.messages[i].contentType) {
          messages[i].setContentType(contentType);
        }
        messages[i].setAttempts(p.messages[i].attempts);
      }
    }
  }

  return req.send().then([this](auto resp) {
    auto respResult = resp.getResult();
    this->result.ackAll = respResult.getAckAll();
    auto retryBatch = respResult.getRetryBatch();
    this->result.retryBatch.retry = retryBatch.getRetry();
    if (retryBatch.isDelaySeconds()) {
      this->result.retryBatch.delaySeconds = retryBatch.getDelaySeconds();
    }

    this->result.explicitAcks.clear();
    for (const auto& msgId: respResult.getExplicitAcks()) {
      this->result.explicitAcks.insert(kj::heapString(msgId));
    }
    this->result.retries.clear();
    for (const auto& retry: respResult.getRetryMessages()) {
      auto& entry = this->result.retries.upsert(kj::heapString(retry.getMsgId()), {});
      if (retry.isDelaySeconds()) {
        entry.value.delaySeconds = retry.getDelaySeconds();
      }
    }

    return WorkerInterface::CustomEvent::Result{
      .outcome = respResult.getOutcome(),
    };
  });
}

kj::Array<QueueRetryMessage> QueueCustomEventImpl::getRetryMessages() const {
  auto retryMsgs = kj::heapArrayBuilder<QueueRetryMessage>(result.retries.size());
  for (const auto& entry: result.retries) {
    retryMsgs.add(QueueRetryMessage{
      .msgId = kj::heapString(entry.key), .delaySeconds = entry.value.delaySeconds});
  }
  return retryMsgs.finish();
}

kj::Array<kj::String> QueueCustomEventImpl::getExplicitAcks() const {
  auto ackArray = kj::heapArrayBuilder<kj::String>(result.explicitAcks.size());
  for (const auto& msgId: result.explicitAcks) {
    ackArray.add(kj::heapString(msgId));
  }
  return ackArray.finish();
}

}  // namespace workerd::api
