/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=4 sw=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef Http3Session_H__
#define Http3Session_H__

#include "nsISupportsImpl.h"
#include "mozilla/net/NeqoHttp3Conn.h"
#include "nsAHttpConnection.h"
#include "nsRefPtrHashtable.h"
#include "nsWeakReference.h"
#include "HttpTrafficAnalyzer.h"
#include "mozilla/UniquePtr.h"
#include "nsDeque.h"

namespace mozilla {
namespace net {

class Http3Stream;
class QuicSocketControl;

// IID for the Http3Session interface
#define NS_HTTP3SESSION_IID                          \
  {                                                  \
    0x8fc82aaf, 0xc4ef, 0x46ed, {                    \
      0x89, 0x41, 0x93, 0x95, 0x8f, 0xac, 0x4f, 0x21 \
    }                                                \
  }

class Http3Session final : public nsAHttpTransaction,
                           public nsAHttpConnection,
                           public nsAHttpSegmentReader,
                           public nsAHttpSegmentWriter,
                           public nsITimerCallback {
 public:
  NS_DECLARE_STATIC_IID_ACCESSOR(NS_HTTP3SESSION_IID)

  NS_DECL_ISUPPORTS
  NS_DECL_NSAHTTPTRANSACTION
  NS_DECL_NSAHTTPCONNECTION(mConnection)
  NS_DECL_NSAHTTPSEGMENTREADER
  NS_DECL_NSAHTTPSEGMENTWRITER
  NS_DECL_NSITIMERCALLBACK

  Http3Session();
  nsresult Init(const nsACString& aOrigin, nsISocketTransport* aSocketTransport,
                nsHttpConnection* readerWriter);

  bool IsConnected() const { return mState == CONNECTED; }
  bool IsClosing() const { return (mState == CLOSING || mState == CLOSED); }
  nsresult GetError() const { return mError; }

  nsresult Process();

  bool AddStream(nsAHttpTransaction* aHttpTransaction, int32_t aPriority,
                 nsIInterfaceRequestor* aCallbacks);

  bool CanReuse();

  // TODO: use this.
  bool RoomForMoreStreams() { return mQueuedStreams.GetSize() == 0; }

  // We will let neqo-transport handle connection timeouts.
  uint32_t ReadTimeoutTick(PRIntervalTime now) { return UINT32_MAX; }

  // overload of nsAHttpTransaction
  MOZ_MUST_USE nsresult ReadSegmentsAgain(nsAHttpSegmentReader*, uint32_t,
                                          uint32_t*, bool*) final;
  MOZ_MUST_USE nsresult WriteSegmentsAgain(nsAHttpSegmentWriter*, uint32_t,
                                           uint32_t*, bool*) final;

  bool ResponseTimeoutEnabled() const final { return true; }
  PRIntervalTime ResponseTimeout() final;

  // The folowing functions are used by Http3Stream:
  nsresult TryActivating(const nsACString& aMethod, const nsACString& aScheme,
                         const nsACString& aHost, const nsACString& aPath,
                         const nsACString& aHeaders, uint64_t* aStreamId,
                         Http3Stream* aStream);
  void CloseSendingSide(uint64_t aStreamId);
  nsresult SendRequestBody(uint64_t aStreamId, const char* buf, uint32_t count,
                           uint32_t* countRead);
  nsresult ReadResponseHeaders(uint64_t aStreamId,
                               nsTArray<uint8_t>& aResponseHeaders, bool* aFin);
  nsresult ReadResponseData(uint64_t aStreamId, char* aBuf, uint32_t aCount,
                            uint32_t* aCountWritten, bool* aFin);

  const static uint32_t kDefaultReadAmount = 2048;

  void CloseStream(Http3Stream* aStream,  nsresult aResult);

  void SetCleanShutdown(bool aCleanShutdown) {
    mCleanShutdown = aCleanShutdown;
  }

  PRIntervalTime IdleTime();

  bool TestJoinConnection(const nsACString& hostname, int32_t port);
  bool JoinConnection(const nsACString& hostname, int32_t port);

  void TransactionHasDataToWrite(nsAHttpTransaction* caller) override;

  nsISocketTransport* SocketTransport() { return mSocketTransport; }

  // This function will be called by QuicSocketControl when the certificate
  // verification is done.
  void Authenticated(int32_t aError);

 private:
  ~Http3Session();

  void CloseInternal(bool aCallNeqoClose);
  void Shutdown();

  bool RealJoinConnection(const nsACString& hostname, int32_t port,
                          bool justKidding);

  nsresult ProcessOutput();
  nsresult ProcessInput();
  nsresult ProcessEvents(uint32_t count, uint32_t* countWritten, bool* again);
  nsresult ProcessOutputAndEvents();

  void SetupTimer(uint64_t aTimeout);

  void ResetRecvd(uint64_t aStreamId, Http3AppError aError);

  void QueueStream(Http3Stream* stream);
  void RemoveStreamFromQueues(Http3Stream*);
  void ProcessPending();

  void CallCertVerification();
  void SetSecInfo();

  RefPtr<NeqoHttp3Conn> mHttp3Connection;
  RefPtr<nsAHttpConnection> mConnection;
  nsRefPtrHashtable<nsUint64HashKey, Http3Stream> mStreamIdHash;
  nsRefPtrHashtable<nsPtrHashKey<nsAHttpTransaction>, Http3Stream>
      mStreamTransactionHash;

  nsDeque mReadyForWrite;
  nsDeque mQueuedStreams;

  enum State { INITIALIZING, CONNECTED, CLOSING, CLOSED } mState;

  bool mAuthenticationStarted;
  bool mCleanShutdown;
  bool mGoawayReceived;
  bool mShouldClose;
  nsresult mError;
  bool mBeforeConnectedError;
  uint64_t mCurrentForegroundTabOuterContentWindowId;

  nsTArray<uint8_t> mPacketToSend;

  RefPtr<nsHttpConnection> mSegmentReaderWriter;

  // The underlying socket transport object is needed to propogate some events
  RefPtr<nsISocketTransport> mSocketTransport;

  nsCOMPtr<nsITimer> mTimer;

  nsDataHashtable<nsCStringHashKey, bool> mJoinConnectionCache;

  RefPtr<QuicSocketControl> mSocketControl;
};

NS_DEFINE_STATIC_IID_ACCESSOR(Http3Session, NS_HTTP3SESSION_IID);

}  // namespace net
}  // namespace mozilla

#endif // Http3Session_H__
