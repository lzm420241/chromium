// Copyright (c) 2006-2008 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#if defined(BROWSER_SYNC)

#ifndef CHROME_BROWSER_SYNC_GLUE_HTTP_BRIDGE_H_
#define CHROME_BROWSER_SYNC_GLUE_HTTP_BRIDGE_H_

#include <string>

#include "base/ref_counted.h"
#include "base/waitable_event.h"
#include "chrome/browser/net/url_fetcher.h"
#include "chrome/browser/sync/engine/syncapi.h"
#include "googleurl/src/gurl.h"
#include "net/url_request/url_request_context.h"
#include "testing/gtest/include/gtest/gtest_prod.h"

class MessageLoop;
class HttpBridgeTest;

namespace browser_sync {

// A bridge between the syncer and Chromium HTTP layers.
// Provides a way for the sync backend to use Chromium directly for HTTP
// requests rather than depending on a third party provider (e.g libcurl).
// This is a one-time use bridge. Create one for each request you want to make.
// It is RefCountedThreadSafe because it can PostTask to the io loop, and thus
// needs to stick around across context switches, etc.
class HttpBridge : public base::RefCountedThreadSafe<HttpBridge>,
                   public sync_api::HttpPostProviderInterface,
                   public URLFetcher::Delegate {
 public:
  // A request context used for HTTP requests bridged from the sync backend.
  // A bridged RequestContext has a dedicated in-memory cookie store and does
  // not use a cache. Thus the same type can be used for incognito mode.
  // TODO(timsteele): We subclass here instead of add a factory method on
  // ChromeURLRequestContext because:
  // 1) we want the ability to set_user_agent
  // 2) avoids ifdefs for now
  // 3) not sure we want to strictly follow settings for cookie policy,
  //    accept lang/charset, since changing these could break syncing.
  class RequestContext : public URLRequestContext {
   public:
    // |baseline_context| is used to obtain the accept-language,
    // accept-charsets, and proxy service information for bridged requests.
    // Typically |baseline_context| should be the URLRequestContext of the
    // currently active profile.
    explicit RequestContext(URLRequestContext* baseline_context);
    virtual ~RequestContext();

    // Set the user agent for requests using this context. The default is
    // the browser's UA string.
    void set_user_agent(const std::string& ua) { user_agent_ = ua; }
    bool is_user_agent_set() const { return !user_agent_.empty(); }

    virtual const std::string& GetUserAgent(const GURL& url) const {
      // If the user agent is set explicitly return that, otherwise call the
      // base class method to return default value.
      return user_agent_.empty() ?
          URLRequestContext::GetUserAgent(url) : user_agent_;
    }

    virtual bool AllowSendingCookies(const URLRequest* request) const {
      return false;
    }

   private:
    std::string user_agent_;
    URLRequestContext* baseline_context_;

    DISALLOW_COPY_AND_ASSIGN(RequestContext);
  };

  HttpBridge(RequestContext* context, MessageLoop* io_loop);
  virtual ~HttpBridge();

  // sync_api::HttpPostProvider implementation.
  virtual void SetUserAgent(const char* user_agent);
  virtual void SetExtraRequestHeaders(const char* headers);
  virtual void SetURL(const char* url, int port);
  virtual void SetPostPayload(const char* content_type, int content_length,
                              const char* content);
  virtual bool MakeSynchronousPost(int* os_error_code, int* response_code);

  // WARNING: these response content methods are used to extract plain old data
  // and not null terminated strings, so you should make sure you have read
  // GetResponseContentLength() characters when using GetResponseContent. e.g
  // string r(b->GetResponseContent(), b->GetResponseContentLength()).
  virtual int GetResponseContentLength() const;
  virtual const char* GetResponseContent() const;

  // URLFetcher::Delegate implementation.
  virtual void OnURLFetchComplete(const URLFetcher* source, const GURL& url,
                                  const URLRequestStatus& status,
                                  int response_code,
                                  const ResponseCookies& cookies,
                                  const std::string& data);

  URLRequestContext* GetRequestContext() const;

 protected:
  // Protected virtual so the unit test can override to shunt network requests.
  virtual void MakeAsynchronousPost();

 private:
  friend class ::HttpBridgeTest;

  // Called on the io_loop_ to issue the network request. The extra level
  // of indirection is so that the unit test can override this behavior but we
  // still have a function to statically pass to PostTask.
  void CallMakeAsynchronousPost() { MakeAsynchronousPost(); }

  // A customized URLRequestContext for bridged requests. See RequestContext
  // definition for details.
  RequestContext* context_for_request_;

  // Our hook into the network layer is a URLFetcher. USED ONLY ON THE IO LOOP,
  // so we can block created_on_loop_ while the fetch is in progress.
  // NOTE: This is not a scoped_ptr for a reason. It must be deleted on the same
  // thread that created it, which isn't the same thread |this| gets deleted on.
  // We must manually delete url_poster_ on the io_loop_.
  URLFetcher* url_poster_;

  // The message loop of the thread we were created on. This is the thread that
  // will block on MakeSynchronousPost while the IO thread fetches data from
  // the network.
  // This should be the main syncer thread (SyncerThread) which is what blocks
  // on network IO through curl_easy_perform.
  MessageLoop* const created_on_loop_;

  // Member variable for the IO loop instead of asking ChromeThread directly,
  // done this way for testability.
  MessageLoop* const io_loop_;

  // The URL to POST to.
  GURL url_for_request_;

  // POST payload information.
  std::string content_type_;
  std::string request_content_;
  std::string extra_headers_;

  // Cached response data.
  bool request_completed_;
  bool request_succeeded_;
  int http_response_code_;
  int os_error_code_;
  std::string response_content_;

  // A waitable event we use to provide blocking semantics to
  // MakeSynchronousPost. We block created_on_loop_ while the io_loop_ fetches
  // network request.
  base::WaitableEvent http_post_completed_;

  // This is here so that the unit test subclass can force our URLFetcher to
  // use the io_loop_ passed on construction for network requests, rather than
  // ChromeThread::IO's message loop (which won't exist in testing).
  bool use_io_loop_for_testing_;

  DISALLOW_COPY_AND_ASSIGN(HttpBridge);
};

class HttpBridgeFactory
  : public sync_api::HttpPostProviderFactory {
 public:
  explicit HttpBridgeFactory(URLRequestContext* baseline_context);
  virtual ~HttpBridgeFactory();
  virtual sync_api::HttpPostProviderInterface* Create();
  virtual void Destroy(sync_api::HttpPostProviderInterface* http);
 private:
  // This request context is built on top of the baseline context and shares
  // common components.
  HttpBridge::RequestContext* GetRequestContext();
  // We must Release() this from the IO thread.
  HttpBridge::RequestContext* request_context_;
  DISALLOW_COPY_AND_ASSIGN(HttpBridgeFactory);
};

}  //  namespace browser_sync

#endif  // CHROME_BROWSER_SYNC_GLUE_HTTP_BRIDGE_H_

#endif  // defined(BROWSER_SYNC)
