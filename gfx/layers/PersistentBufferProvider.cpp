/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "PersistentBufferProvider.h"

#include "Layers.h"
#include "mozilla/layers/ShadowLayers.h"
#include "mozilla/layers/TextureClient.h"
#include "mozilla/gfx/gfxVars.h"
#include "mozilla/gfx/Logging.h"
#include "mozilla/StaticPrefs_layers.h"
#include "pratom.h"
#include "gfxPlatform.h"

namespace mozilla {

using namespace gfx;

namespace layers {

PersistentBufferProviderBasic::PersistentBufferProviderBasic(DrawTarget* aDt)
    : mDrawTarget(aDt) {
  MOZ_COUNT_CTOR(PersistentBufferProviderBasic);
}

PersistentBufferProviderBasic::~PersistentBufferProviderBasic() {
  MOZ_COUNT_DTOR(PersistentBufferProviderBasic);
  Destroy();
}

already_AddRefed<gfx::DrawTarget>
PersistentBufferProviderBasic::BorrowDrawTarget(
    const gfx::IntRect& aPersistedRect) {
  MOZ_ASSERT(!mSnapshot);
  RefPtr<gfx::DrawTarget> dt(mDrawTarget);
  return dt.forget();
}

bool PersistentBufferProviderBasic::ReturnDrawTarget(
    already_AddRefed<gfx::DrawTarget> aDT) {
  RefPtr<gfx::DrawTarget> dt(aDT);
  MOZ_ASSERT(mDrawTarget == dt);
  if (dt) {
    // Since SkiaGL default to storing drawing command until flush
    // we have to flush it before present.
    dt->Flush();
  }
  return true;
}

already_AddRefed<gfx::SourceSurface>
PersistentBufferProviderBasic::BorrowSnapshot() {
  mSnapshot = mDrawTarget->Snapshot();
  RefPtr<SourceSurface> snapshot = mSnapshot;
  return snapshot.forget();
}

void PersistentBufferProviderBasic::ReturnSnapshot(
    already_AddRefed<gfx::SourceSurface> aSnapshot) {
  RefPtr<SourceSurface> snapshot = aSnapshot;
  MOZ_ASSERT(!snapshot || snapshot == mSnapshot);
  mSnapshot = nullptr;
}

void PersistentBufferProviderBasic::Destroy() {
  mSnapshot = nullptr;
  mDrawTarget = nullptr;
}

// static
already_AddRefed<PersistentBufferProviderBasic>
PersistentBufferProviderBasic::Create(gfx::IntSize aSize,
                                      gfx::SurfaceFormat aFormat,
                                      gfx::BackendType aBackend) {
  RefPtr<DrawTarget> dt =
      gfxPlatform::GetPlatform()->CreateDrawTargetForBackend(aBackend, aSize,
                                                             aFormat);

  if (dt) {
    // This is simply to ensure the DrawTarget gets initialized, and will detect
    // a device reset, even if we're on the main thread.
    dt->ClearRect(Rect(0, 0, 0, 0));
  }

  if (!dt || !dt->IsValid()) {
    return nullptr;
  }

  RefPtr<PersistentBufferProviderBasic> provider =
      new PersistentBufferProviderBasic(dt);

  return provider.forget();
}

// static
already_AddRefed<PersistentBufferProviderShared>
PersistentBufferProviderShared::Create(gfx::IntSize aSize,
                                       gfx::SurfaceFormat aFormat,
                                       KnowsCompositor* aKnowsCompositor) {
  if (!aKnowsCompositor ||
      !aKnowsCompositor->GetTextureForwarder() ||
      !aKnowsCompositor->GetTextureForwarder()->IPCOpen() ||
      // Bug 1556433 - shared buffer provider and direct texture mapping do not
      // synchronize properly
      aKnowsCompositor->SupportsTextureDirectMapping()) {
    return nullptr;
  }

  if (!StaticPrefs::layers_shared_buffer_provider_enabled()) {
    return nullptr;
  }

#ifdef XP_WIN
  // Bug 1285271 - Disable shared buffer provider on Windows with D2D due to
  // instability, unless we are remoting the canvas drawing to the GPU process.
  if (gfxPlatform::GetPlatform()->GetPreferredCanvasBackend() ==
          BackendType::DIRECT2D1_1 &&
      !TextureData::IsRemote(aKnowsCompositor->GetCompositorBackendType(),
                             BackendSelector::Canvas)) {
    return nullptr;
  }
#endif

  RefPtr<TextureClient> texture = TextureClient::CreateForDrawing(
      aKnowsCompositor, aFormat, aSize, BackendSelector::Canvas,
      TextureFlags::DEFAULT | TextureFlags::NON_BLOCKING_READ_LOCK,
      TextureAllocationFlags::ALLOC_DEFAULT);

  if (!texture) {
    return nullptr;
  }

  RefPtr<PersistentBufferProviderShared> provider =
      new PersistentBufferProviderShared(aSize, aFormat, aKnowsCompositor,
                                         texture);
  return provider.forget();
}

PersistentBufferProviderShared::PersistentBufferProviderShared(
    gfx::IntSize aSize, gfx::SurfaceFormat aFormat,
    KnowsCompositor* aKnowsCompositor, RefPtr<TextureClient>& aTexture)

    : mSize(aSize),
      mFormat(aFormat),
      mKnowsCompositor(aKnowsCompositor),
      mFront(Nothing()) {
  MOZ_ASSERT(aKnowsCompositor);
  if (mTextures.append(aTexture)) {
    mBack = Some<uint32_t>(0);
  }

  // If we are using webrender and our textures don't have an intermediate
  // buffer, then we have to hold onto the textures for longer to make sure that
  // the GPU has finished using them. So, we need to allow more TextureClients
  // to be created.
  if (!aTexture->HasIntermediateBuffer() && gfxVars::UseWebRender()) {
    ++mMaxAllowedTextures;
    if (gfxVars::UseWebRenderTripleBufferingWin()) {
      ++mMaxAllowedTextures;
    }
  }

  MOZ_COUNT_CTOR(PersistentBufferProviderShared);
}

PersistentBufferProviderShared::~PersistentBufferProviderShared() {
  MOZ_COUNT_DTOR(PersistentBufferProviderShared);

  if (IsActivityTracked()) {
    mKnowsCompositor->GetActiveResourceTracker()->RemoveObject(this);
  }

  Destroy();
}

LayersBackend PersistentBufferProviderShared::GetType() {
  if (mKnowsCompositor->GetCompositorBackendType() ==
      LayersBackend::LAYERS_WR) {
    return LayersBackend::LAYERS_WR;
  } else {
    return LayersBackend::LAYERS_CLIENT;
  }
}

bool PersistentBufferProviderShared::SetKnowsCompositor(
    KnowsCompositor* aKnowsCompositor) {
  MOZ_ASSERT(aKnowsCompositor);
  if (!aKnowsCompositor) {
    return false;
  }

  if (mKnowsCompositor == aKnowsCompositor) {
    // The forwarder should not change most of the time.
    return true;
  }

  if (IsActivityTracked()) {
    mKnowsCompositor->GetActiveResourceTracker()->RemoveObject(this);
  }

  if (mKnowsCompositor->GetTextureForwarder() !=
          aKnowsCompositor->GetTextureForwarder() ||
      mKnowsCompositor->GetCompositorBackendType() !=
          aKnowsCompositor->GetCompositorBackendType()) {
    // We are going to be used with an different and/or incompatible forwarder.
    // This should be extremely rare. We have to copy the front buffer into a
    // texture that is compatible with the new forwarder.

    // Grab the current front buffer.
    RefPtr<TextureClient> prevTexture = GetTexture(mFront);

    // Get rid of everything else
    Destroy();

    if (prevTexture) {
      RefPtr<TextureClient> newTexture = TextureClient::CreateForDrawing(
          aKnowsCompositor, mFormat, mSize, BackendSelector::Canvas,
          TextureFlags::DEFAULT | TextureFlags::NON_BLOCKING_READ_LOCK,
          TextureAllocationFlags::ALLOC_DEFAULT);

      MOZ_ASSERT(newTexture);
      if (!newTexture) {
        return false;
      }

      // If we early-return in one of the following branches, we will
      // leave the buffer provider in an empty state, since we called
      // Destroy. Not ideal but at least we won't try to use it with a
      // an incompatible ipc channel.

      if (!newTexture->Lock(OpenMode::OPEN_WRITE)) {
        return false;
      }

      if (!prevTexture->Lock(OpenMode::OPEN_READ)) {
        newTexture->Unlock();
        return false;
      }

      bool success =
          prevTexture->CopyToTextureClient(newTexture, nullptr, nullptr);

      prevTexture->Unlock();
      newTexture->Unlock();

      if (!success) {
        return false;
      }

      if (!mTextures.append(newTexture)) {
        return false;
      }
      mFront = Some<uint32_t>(mTextures.length() - 1);
      mBack = mFront;
    }
  }

  mKnowsCompositor = aKnowsCompositor;

  return true;
}

TextureClient* PersistentBufferProviderShared::GetTexture(
    const Maybe<uint32_t>& aIndex) {
  if (aIndex.isNothing() || !CheckIndex(aIndex.value())) {
    return nullptr;
  }
  return mTextures[aIndex.value()];
}

already_AddRefed<gfx::DrawTarget>
PersistentBufferProviderShared::BorrowDrawTarget(
    const gfx::IntRect& aPersistedRect) {
  if (!mKnowsCompositor->GetTextureForwarder() ||
      !mKnowsCompositor->GetTextureForwarder()->IPCOpen()) {
    return nullptr;
  }

  MOZ_ASSERT(!mSnapshot);

  if (IsActivityTracked()) {
    mKnowsCompositor->GetActiveResourceTracker()->MarkUsed(this);
  } else {
    mKnowsCompositor->GetActiveResourceTracker()->AddObject(this);
  }

  if (mDrawTarget) {
    RefPtr<gfx::DrawTarget> dt(mDrawTarget);
    return dt.forget();
  }

  auto previousBackBuffer = mBack;

  TextureClient* tex = GetTexture(mBack);

  // First try to reuse the current back buffer. If we can do that it means
  // we can skip copying its content to the new back buffer.
  if ((mTextureLockIsUnreliable.isSome() &&
       mTextureLockIsUnreliable == mBack) ||
      (tex && tex->IsReadLocked())) {
    // The back buffer is currently used by the compositor, we can't draw
    // into it.
    tex = nullptr;
  }

  if (!tex) {
    // Try to grab an already allocated texture if any is available.
    for (uint32_t i = 0; i < mTextures.length(); ++i) {
      if (!mTextures[i]->IsReadLocked() &&
          !(mTextureLockIsUnreliable.isSome() &&
            mTextureLockIsUnreliable.ref() == i)) {
        mBack = Some(i);
        tex = mTextures[i];
        break;
      }
    }
  }

  if (!tex) {
    // We have to allocate a new texture.
    if (mTextures.length() >= mMaxAllowedTextures) {
      // We should never need to buffer that many textures, something's wrong.
      // In theory we throttle the main thread when the compositor can't keep
      // up, so we shoud never get in a situation where we sent 4 textures to
      // the compositor and the latter has not released any of them. In
      // practice, though, the throttling mechanism appears to have some issues,
      // especially when switching between layer managers (during tab-switch).
      // To make sure we don't get too far ahead of the compositor, we send a
      // sync ping to the compositor thread...
      mKnowsCompositor->SyncWithCompositor();
      // ...and try again.
      for (uint32_t i = 0; i < mTextures.length(); ++i) {
        if (!mTextures[i]->IsReadLocked()) {
          gfxCriticalNote << "Managed to allocate after flush.";
          mBack = Some(i);
          tex = mTextures[i];
          break;
        }
      }

      if (!tex) {
        gfxCriticalNote << "Unexpected BufferProvider over-production.";
        // It would be pretty bad to keep piling textures up at this point so we
        // call NotifyInactive to remove some of our textures.
        NotifyInactive();
        // Give up now. The caller can fall-back to a non-shared buffer
        // provider.
        return nullptr;
      }
    }

    RefPtr<TextureClient> newTexture = TextureClient::CreateForDrawing(
        mKnowsCompositor, mFormat, mSize, BackendSelector::Canvas,
        TextureFlags::DEFAULT | TextureFlags::NON_BLOCKING_READ_LOCK,
        TextureAllocationFlags::ALLOC_DEFAULT);

    MOZ_ASSERT(newTexture);
    if (newTexture) {
      if (mTextures.append(newTexture)) {
        tex = newTexture;
        mBack = Some<uint32_t>(mTextures.length() - 1);
      }
    }
  }

  if (!tex || !tex->Lock(OpenMode::OPEN_READ_WRITE)) {
    return nullptr;
  }

  // Clear dirty texture, since new back texture is selected.
  mTextureLockIsUnreliable = Nothing();

  mDrawTarget = tex->BorrowDrawTarget();
  if (mBack != previousBackBuffer && !aPersistedRect.IsEmpty()) {
    if (mPreviousSnapshot) {
      mDrawTarget->CopySurface(mPreviousSnapshot, aPersistedRect,
                               gfx::IntPoint(0, 0));
    } else {
      TextureClient* previous = GetTexture(previousBackBuffer);
      if (previous && previous->Lock(OpenMode::OPEN_READ)) {
        DebugOnly<bool> success =
            previous->CopyToTextureClient(tex, &aPersistedRect, nullptr);
        MOZ_ASSERT(success);

        previous->Unlock();
      }
    }
  }
  mPreviousSnapshot = nullptr;

  if (mDrawTarget) {
    // This is simply to ensure the DrawTarget gets initialized, and will detect
    // a device reset, even if we're on the main thread.
    mDrawTarget->ClearRect(Rect(0, 0, 0, 0));

    if (!mDrawTarget->IsValid()) {
      mDrawTarget = nullptr;
    }
  }

  RefPtr<gfx::DrawTarget> dt(mDrawTarget);
  return dt.forget();
}

bool PersistentBufferProviderShared::ReturnDrawTarget(
    already_AddRefed<gfx::DrawTarget> aDT) {
  RefPtr<gfx::DrawTarget> dt(aDT);
  MOZ_ASSERT(mDrawTarget == dt);
  // Can't change the current front buffer while its snapshot is borrowed!
  MOZ_ASSERT(!mSnapshot);

  TextureClient* back = GetTexture(mBack);
  MOZ_ASSERT(back);

  // If our TextureClients have internal synchronization then, if locks are
  // needed for reading and writing, this can cause locking issues with the
  // compositor. To prevent this we take a snapshot when the DrawTarget is
  // returned, so this can be used when our own BorrowSnapshot is called and
  // also for copying to the next TextureClient. Using this snapshot outside of
  // the locks is safe, because the TextureClient calls DetachAllSnapshots on
  // its DrawTarget when we Unlock below.
  if (back->HasSynchronization()) {
    mPreviousSnapshot = back->BorrowSnapshot();
  }

  mDrawTarget = nullptr;
  dt = nullptr;

  if (back) {
    back->Unlock();
    mFront = mBack;
  }

  return !!back;
}

TextureClient* PersistentBufferProviderShared::GetTextureClient() {
  // Can't access the front buffer while drawing.
  MOZ_ASSERT(!mDrawTarget);
  TextureClient* texture = GetTexture(mFront);
  if (!texture) {
    gfxCriticalNote
        << "PersistentBufferProviderShared: front buffer unavailable";
  }
  return texture;
}

already_AddRefed<gfx::SourceSurface>
PersistentBufferProviderShared::BorrowSnapshot() {
  if (mPreviousSnapshot) {
    mSnapshot = mPreviousSnapshot;
    return do_AddRef(mSnapshot);
  }

  if (mDrawTarget) {
    auto back = GetTexture(mBack);
    MOZ_ASSERT(back && back->IsLocked());
    mSnapshot = back->BorrowSnapshot();
    return do_AddRef(mSnapshot);
  }

  auto front = GetTexture(mFront);
  if (!front || front->IsLocked()) {
    MOZ_ASSERT(false);
    return nullptr;
  }

  if (!front->Lock(OpenMode::OPEN_READ)) {
    return nullptr;
  }

  mSnapshot = front->BorrowSnapshot();

  return do_AddRef(mSnapshot);
}

void PersistentBufferProviderShared::ReturnSnapshot(
    already_AddRefed<gfx::SourceSurface> aSnapshot) {
  RefPtr<SourceSurface> snapshot = aSnapshot;
  MOZ_ASSERT(!snapshot || snapshot == mSnapshot);

  mSnapshot = nullptr;
  snapshot = nullptr;

  if (mPreviousSnapshot || mDrawTarget) {
    return;
  }

  auto front = GetTexture(mFront);
  if (front) {
    front->Unlock();
  }
}

void PersistentBufferProviderShared::NotifyInactive() {
  ClearCachedResources();
}

void PersistentBufferProviderShared::ClearCachedResources() {
  RefPtr<TextureClient> front = GetTexture(mFront);
  RefPtr<TextureClient> back = GetTexture(mBack);

  // Clear all textures (except the front and back ones that we just kept).
  mTextures.clear();

  if (back) {
    if (mTextures.append(back)) {
      mBack = Some<uint32_t>(0);
    }
    if (front == back) {
      mFront = mBack;
    }
  }

  if (front && front != back) {
    if (mTextures.append(front)) {
      mFront = Some<uint32_t>(mTextures.length() - 1);
    }
  }
  // Set front texture as dirty texture.
  // The texture's read lock is unreliable after this function call.
  mTextureLockIsUnreliable = mFront;
}

void PersistentBufferProviderShared::Destroy() {
  mSnapshot = nullptr;
  mPreviousSnapshot = nullptr;
  mDrawTarget = nullptr;

  for (auto& mTexture : mTextures) {
    TextureClient* texture = mTexture;
    if (texture && texture->IsLocked()) {
      MOZ_ASSERT(false);
      texture->Unlock();
    }
  }

  mTextures.clear();
}

}  // namespace layers
}  // namespace mozilla
