/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AntiTrackingLog.h"
#include "ContentBlocking.h"
#include "AntiTrackingUtils.h"
#include "TemporaryAccessGrantObserver.h"

#include "mozilla/ContentBlockingAllowList.h"
#include "mozilla/ContentBlockingUserInteraction.h"
#include "mozilla/dom/BrowsingContext.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/StaticPrefs_privacy.h"
#include "mozIThirdPartyUtil.h"
#include "nsContentUtils.h"
#include "nsGlobalWindowInner.h"
#include "nsIClassifiedChannel.h"
#include "nsICookiePermission.h"
#include "nsICookieService.h"
#include "nsIPermission.h"
#include "nsIPrincipal.h"
#include "nsIURI.h"
#include "nsIOService.h"
#include "nsIWebProgressListener.h"
#include "nsPermissionManager.h"
#include "nsScriptSecurityManager.h"

namespace mozilla {

LazyLogModule gAntiTrackingLog("AntiTracking");

}

using namespace mozilla;
using mozilla::dom::BrowsingContext;
using mozilla::dom::ContentChild;
using mozilla::dom::Document;

namespace {

bool GetParentPrincipalAndTrackingOrigin(
    nsGlobalWindowInner* a3rdPartyTrackingWindow, uint32_t aBehavior,
    nsIPrincipal** aTopLevelStoragePrincipal, nsACString& aTrackingOrigin,
    nsIPrincipal** aTrackingPrincipal) {
  // Now we need the principal and the origin of the parent window.
  nsCOMPtr<nsIPrincipal> topLevelStoragePrincipal =
      // Use the "top-level storage area principal" behaviour in reject tracker
      // mode only.
      (aBehavior == nsICookieService::BEHAVIOR_REJECT_TRACKER)
          ? a3rdPartyTrackingWindow->GetTopLevelStorageAreaPrincipal()
          : a3rdPartyTrackingWindow->GetTopLevelAntiTrackingPrincipal();
  if (!topLevelStoragePrincipal) {
    LOG(("No top-level storage area principal at hand"));
    return false;
  }

  // Let's take the principal and the origin of the tracker.
  nsCOMPtr<nsIPrincipal> trackingPrincipal =
      a3rdPartyTrackingWindow->GetPrincipal();
  if (NS_WARN_IF(!trackingPrincipal)) {
    return false;
  }

  nsresult rv = trackingPrincipal->GetOriginNoSuffix(aTrackingOrigin);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return false;
  }

  topLevelStoragePrincipal.forget(aTopLevelStoragePrincipal);
  if (aTrackingPrincipal) {
    trackingPrincipal.forget(aTrackingPrincipal);
  }
  return true;
};

// This internal method returns ACCESS_DENY if the access is denied,
// ACCESS_DEFAULT if unknown, some other access code if granted.
uint32_t CheckCookiePermissionForPrincipal(
    nsICookieJarSettings* aCookieJarSettings, nsIPrincipal* aPrincipal) {
  MOZ_ASSERT(aCookieJarSettings);
  MOZ_ASSERT(aPrincipal);

  uint32_t cookiePermission = nsICookiePermission::ACCESS_DEFAULT;
  if (!aPrincipal->GetIsContentPrincipal()) {
    return cookiePermission;
  }

  nsresult rv =
      aCookieJarSettings->CookiePermission(aPrincipal, &cookiePermission);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return nsICookiePermission::ACCESS_DEFAULT;
  }

  // If we have a custom cookie permission, let's use it.
  return cookiePermission;
}

int32_t CookiesBehavior(Document* a3rdPartyDocument) {
  MOZ_ASSERT(a3rdPartyDocument);

  // WebExtensions principals always get BEHAVIOR_ACCEPT as cookieBehavior
  // (See Bug 1406675 and Bug 1525917 for rationale).
  if (BasePrincipal::Cast(a3rdPartyDocument->NodePrincipal())->AddonPolicy()) {
    return nsICookieService::BEHAVIOR_ACCEPT;
  }

  return a3rdPartyDocument->CookieJarSettings()->GetCookieBehavior();
}

int32_t CookiesBehavior(nsILoadInfo* aLoadInfo, nsIURI* a3rdPartyURI) {
  MOZ_ASSERT(aLoadInfo);
  MOZ_ASSERT(a3rdPartyURI);

  // WebExtensions 3rd party URI always get BEHAVIOR_ACCEPT as cookieBehavior,
  // this is semantically equivalent to the principal having a AddonPolicy().
  if (a3rdPartyURI->SchemeIs("moz-extension")) {
    return nsICookieService::BEHAVIOR_ACCEPT;
  }

  nsCOMPtr<nsICookieJarSettings> cookieJarSettings;
  nsresult rv =
      aLoadInfo->GetCookieJarSettings(getter_AddRefs(cookieJarSettings));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    return nsICookieService::BEHAVIOR_REJECT;
  }

  return cookieJarSettings->GetCookieBehavior();
}

int32_t CookiesBehavior(nsIPrincipal* aPrincipal,
                        nsICookieJarSettings* aCookieJarSettings) {
  MOZ_ASSERT(aPrincipal);
  MOZ_ASSERT(aCookieJarSettings);

  // WebExtensions principals always get BEHAVIOR_ACCEPT as cookieBehavior
  // (See Bug 1406675 for rationale).
  if (BasePrincipal::Cast(aPrincipal)->AddonPolicy()) {
    return nsICookieService::BEHAVIOR_ACCEPT;
  }

  return aCookieJarSettings->GetCookieBehavior();
}

bool CheckAntiTrackingPermission(nsIPrincipal* aPrincipal,
                                 const nsAutoCString& aType,
                                 bool aIsInPrivateBrowsing,
                                 uint32_t* aRejectedReason,
                                 uint32_t aBlockedReason) {
  nsPermissionManager* permManager = nsPermissionManager::GetInstance();
  if (NS_WARN_IF(!permManager)) {
    LOG(("Failed to obtain the permission manager"));
    return false;
  }

  uint32_t result = 0;
  if (aIsInPrivateBrowsing) {
    LOG_PRIN(("Querying the permissions for private modei looking for a "
              "permission of type %s for %s",
              aType.get(), _spec),
             aPrincipal);
    if (!permManager->PermissionAvailable(aPrincipal, aType)) {
      LOG(
          ("Permission isn't available for this principal in the current "
           "process"));
      return false;
    }
    nsTArray<RefPtr<nsIPermission>> permissions;
    nsresult rv = permManager->GetAllForPrincipal(aPrincipal, permissions);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      LOG(("Failed to get the list of permissions"));
      return false;
    }

    bool found = false;
    for (const auto& permission : permissions) {
      if (!permission) {
        LOG(("Couldn't get the permission for unknown reasons"));
        continue;
      }

      nsAutoCString permissionType;
      if (NS_SUCCEEDED(permission->GetType(permissionType)) &&
          permissionType != aType) {
        LOG(("Non-matching permission type: %s", aType.get()));
        continue;
      }

      uint32_t capability = 0;
      if (NS_SUCCEEDED(permission->GetCapability(&capability)) &&
          capability != nsIPermissionManager::ALLOW_ACTION) {
        LOG(("Non-matching permission capability: %d", capability));
        continue;
      }

      uint32_t expirationType = 0;
      if (NS_SUCCEEDED(permission->GetExpireType(&expirationType)) &&
          expirationType != nsIPermissionManager ::EXPIRE_SESSION) {
        LOG(("Non-matching permission expiration type: %d", expirationType));
        continue;
      }

      int64_t expirationTime = 0;
      if (NS_SUCCEEDED(permission->GetExpireTime(&expirationTime)) &&
          expirationTime != 0) {
        LOG(("Non-matching permission expiration time: %" PRId64,
             expirationTime));
        continue;
      }

      LOG(("Found a matching permission"));
      found = true;
      break;
    }

    if (!found) {
      if (aRejectedReason) {
        *aRejectedReason = aBlockedReason;
      }
      return false;
    }
  } else {
    nsresult rv = permManager->TestPermissionWithoutDefaultsFromPrincipal(
        aPrincipal, aType, &result);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      LOG(("Failed to test the permission"));
      return false;
    }

    LOG_PRIN(
        ("Testing permission type %s for %s resulted in %d (%s)", aType.get(),
         _spec, int(result),
         result == nsIPermissionManager::ALLOW_ACTION ? "success" : "failure"),
        aPrincipal);

    if (result != nsIPermissionManager::ALLOW_ACTION) {
      if (aRejectedReason) {
        *aRejectedReason = aBlockedReason;
      }
      return false;
    }
  }

  return true;
}

}  // namespace

/* static */ RefPtr<ContentBlocking::StorageAccessGrantPromise>
ContentBlocking::AllowAccessFor(
    nsIPrincipal* aPrincipal, dom::BrowsingContext* aParentContext,
    ContentBlockingNotifier::StorageAccessGrantedReason aReason,
    const ContentBlocking::PerformFinalChecks& aPerformFinalChecks) {
  MOZ_ASSERT(aParentContext);

  switch (aReason) {
    case ContentBlockingNotifier::eOpener:
      if (!StaticPrefs::
              privacy_restrict3rdpartystorage_heuristic_window_open()) {
        LOG(
            ("Bailing out early because the "
             "privacy.restrict3rdpartystorage.heuristic.window_open preference "
             "has been disabled"));
        return StorageAccessGrantPromise::CreateAndReject(false, __func__);
      }
      break;
    case ContentBlockingNotifier::eOpenerAfterUserInteraction:
      if (!StaticPrefs::
              privacy_restrict3rdpartystorage_heuristic_opened_window_after_interaction()) {
        LOG(
            ("Bailing out early because the "
             "privacy.restrict3rdpartystorage.heuristic.opened_window_after_"
             "interaction preference has been disabled"));
        return StorageAccessGrantPromise::CreateAndReject(false, __func__);
      }
      break;
    default:
      break;
  }

  if (MOZ_LOG_TEST(gAntiTrackingLog, mozilla::LogLevel::Debug)) {
    nsAutoCString origin;
    aPrincipal->GetAsciiOrigin(origin);
    LOG(("Adding a first-party storage exception for %s...",
         PromiseFlatCString(origin).get()));
  }

  nsCOMPtr<nsPIDOMWindowOuter> parentOuter = aParentContext->GetDOMWindow();
  if (!parentOuter) {
    // TODO: Bug 1616775 should implement the parent version of AllowAccessFor
    // here when parent window is NOT in-process.
    LOG(
        ("No outer window found for our parent window context, bailing out "
         "early"));
    return StorageAccessGrantPromise::CreateAndReject(false, __func__);
  }

  nsCOMPtr<nsPIDOMWindowInner> parentInner =
      parentOuter->GetCurrentInnerWindow();
  if (!parentInner) {
    LOG(
        ("No inner window found for our parent outer window, bailing out "
         "early"));
    return StorageAccessGrantPromise::CreateAndReject(false, __func__);
  }

  Document* parentDoc = parentInner->GetExtantDoc();
  if (!parentDoc) {
    LOG(("No document found for our parent inner window, bailing out early"));
    return StorageAccessGrantPromise::CreateAndReject(false, __func__);
  }
  int32_t behavior = parentDoc->CookieJarSettings()->GetCookieBehavior();

  if (!parentDoc->CookieJarSettings()->GetRejectThirdPartyTrackers()) {
    LOG(
        ("Disabled by network.cookie.cookieBehavior pref (%d), bailing out "
         "early",
         behavior));
    return StorageAccessGrantPromise::CreateAndResolve(true, __func__);
  }

  MOZ_ASSERT(
      behavior == nsICookieService::BEHAVIOR_REJECT_TRACKER ||
      behavior ==
          nsICookieService::BEHAVIOR_REJECT_TRACKER_AND_PARTITION_FOREIGN);

  if (ContentBlockingAllowList::Check(parentInner)) {
    return StorageAccessGrantPromise::CreateAndResolve(true, __func__);
  }

  nsCOMPtr<nsIPrincipal> topLevelStoragePrincipal;
  nsAutoCString trackingOrigin;
  nsCOMPtr<nsIPrincipal> trackingPrincipal;

  RefPtr<nsGlobalWindowInner> parentWindow =
      nsGlobalWindowInner::Cast(parentInner);
  nsGlobalWindowOuter* outerParentWindow =
      nsGlobalWindowOuter::Cast(parentWindow->GetOuterWindow());
  if (NS_WARN_IF(!outerParentWindow)) {
    LOG(("No outer window found for our parent window, bailing out early"));
    return StorageAccessGrantPromise::CreateAndReject(false, __func__);
  }

  LOG(("The current resource is %s-party",
       outerParentWindow->IsTopLevelWindow() ? "first" : "third"));

  nsresult rv;
  // We are a first party resource.
  if (outerParentWindow->IsTopLevelWindow()) {
    nsAutoCString origin;
    rv = aPrincipal->GetAsciiOrigin(origin);
    if (NS_WARN_IF(NS_FAILED(rv))) {
      LOG(("Can't get the origin from the URI"));
      return StorageAccessGrantPromise::CreateAndReject(false, __func__);
    }

    trackingOrigin = origin;
    trackingPrincipal = aPrincipal;
    topLevelStoragePrincipal = parentWindow->GetPrincipal();
    if (NS_WARN_IF(!topLevelStoragePrincipal)) {
      LOG(("Top-level storage area principal not found, bailing out early"));
      return StorageAccessGrantPromise::CreateAndReject(false, __func__);
    }

  } else {
    // We should be a 3rd party source.
    bool isThirdParty = false;
    if (behavior == nsICookieService::BEHAVIOR_REJECT_TRACKER) {
      isThirdParty =
          nsContentUtils::IsThirdPartyTrackingResourceWindow(parentWindow);
    } else if (behavior == nsICookieService::
                               BEHAVIOR_REJECT_TRACKER_AND_PARTITION_FOREIGN) {
      isThirdParty = nsContentUtils::IsThirdPartyWindowOrChannel(
          parentWindow, nullptr, nullptr);
    }

    if (!isThirdParty) {
      if (behavior == nsICookieService::BEHAVIOR_REJECT_TRACKER) {
        LOG(("Our window isn't a third-party tracking window"));
      } else if (behavior ==
                 nsICookieService::
                     BEHAVIOR_REJECT_TRACKER_AND_PARTITION_FOREIGN) {
        LOG(("Our window isn't a third-party window"));
      }
      return StorageAccessGrantPromise::CreateAndReject(false, __func__);
    }

    Document* doc = parentWindow->GetExtantDoc();
    // Make sure storage access isn't disabled
    if (doc && (doc->StorageAccessSandboxed())) {
      LOG(("Our document is sandboxed"));
      return StorageAccessGrantPromise::CreateAndReject(false, __func__);
    }

    if (!GetParentPrincipalAndTrackingOrigin(
            parentWindow,
            // Don't request the ETP specific behaviour of allowing only
            // singly-nested iframes here, because we are recording an allow
            // permission.
            nsICookieService::BEHAVIOR_ACCEPT,
            getter_AddRefs(topLevelStoragePrincipal), trackingOrigin,
            getter_AddRefs(trackingPrincipal))) {
      LOG(
          ("Error while computing the parent principal and tracking origin, "
           "bailing out early"));
      return StorageAccessGrantPromise::CreateAndReject(false, __func__);
    }
  }

  nsPIDOMWindowOuter* topOuterWindow = aParentContext->Top()->GetDOMWindow();
  nsGlobalWindowOuter* topWindow = nsGlobalWindowOuter::Cast(topOuterWindow);
  if (NS_WARN_IF(!topWindow)) {
    LOG(("No top outer window."));
    return StorageAccessGrantPromise::CreateAndReject(false, __func__);
  }

  nsPIDOMWindowInner* topInnerWindow = topWindow->GetCurrentInnerWindow();
  if (NS_WARN_IF(!topInnerWindow)) {
    LOG(("No top inner window."));
    return StorageAccessGrantPromise::CreateAndReject(false, __func__);
  }

  // We hardcode this block reason since the first-party storage access
  // permission is granted for the purpose of blocking trackers.
  // Note that if aReason is eOpenerAfterUserInteraction and the
  // trackingPrincipal is not in a blacklist, we don't check the
  // user-interaction state, because it could be that the current process has
  // just sent the request to store the user-interaction permission into the
  // parent, without having received the permission itself yet.
  //
  // We define this as an enum, since without that MSVC fails to capturing this
  // name inside the lambda without the explicit capture and clang warns if
  // there is an explicit capture with -Wunused-lambda-capture.
  enum : uint32_t {
    blockReason = nsIWebProgressListener::STATE_COOKIES_BLOCKED_TRACKER
  };
  bool isInPrefList = false;
  trackingPrincipal->IsURIInPrefList(
      "privacy.restrict3rdpartystorage."
      "userInteractionRequiredForHosts",
      &isInPrefList);
  if (isInPrefList &&
      !ContentBlockingUserInteraction::Exists(trackingPrincipal)) {
    LOG_PRIN(("Tracking principal (%s) hasn't been interacted with before, "
              "refusing to add a first-party storage permission to access it",
              _spec),
             trackingPrincipal);
    ContentBlockingNotifier::OnDecision(
        parentInner, ContentBlockingNotifier::BlockingDecision::eBlock,
        blockReason);
    return StorageAccessGrantPromise::CreateAndReject(false, __func__);
  }

  nsCOMPtr<nsPIDOMWindowOuter> pwin =
      AntiTrackingUtils::GetTopWindow(parentWindow);
  if (!pwin) {
    LOG(("Couldn't get the top window"));
    return StorageAccessGrantPromise::CreateAndReject(false, __func__);
  }

  auto storePermission =
      [pwin, parentWindow, trackingOrigin, trackingPrincipal, topInnerWindow,
       topLevelStoragePrincipal,
       aReason](int aAllowMode) -> RefPtr<StorageAccessGrantPromise> {
    nsAutoCString permissionKey;
    AntiTrackingUtils::CreateStoragePermissionKey(trackingOrigin,
                                                  permissionKey);

    // Let's store the permission in the current parent window.
    topInnerWindow->SaveStorageAccessGranted(permissionKey);

    // Let's inform the parent window.
    parentWindow->StorageAccessGranted();

    nsIChannel* channel =
        pwin->GetCurrentInnerWindow()->GetExtantDoc()->GetChannel();

    ContentBlockingNotifier::OnEvent(
        pwin, channel, parentWindow->GetExtantDoc()->GetChannel(), false,
        blockReason, trackingOrigin, Some(aReason));

    ContentBlockingNotifier::ReportUnblockingToConsole(
        parentWindow, NS_ConvertUTF8toUTF16(trackingOrigin), aReason);

    if (XRE_IsParentProcess()) {
      LOG(("Saving the permission: trackingOrigin=%s", trackingOrigin.get()));
      return SaveAccessForOriginOnParentProcess(topLevelStoragePrincipal,
                                                trackingPrincipal,
                                                trackingOrigin, aAllowMode)
          ->Then(GetCurrentThreadSerialEventTarget(), __func__,
                 [](ParentAccessGrantPromise::ResolveOrRejectValue&& aValue) {
                   if (aValue.IsResolve()) {
                     return StorageAccessGrantPromise::CreateAndResolve(
                         eAllow, __func__);
                   }
                   return StorageAccessGrantPromise::CreateAndReject(false,
                                                                     __func__);
                 });
    }

    ContentChild* cc = ContentChild::GetSingleton();
    MOZ_ASSERT(cc);

    LOG(
        ("Asking the parent process to save the permission for us: "
         "trackingOrigin=%s",
         trackingOrigin.get()));

    // This is not really secure, because here we have the content process
    // sending the request of storing a permission.
    return cc
        ->SendFirstPartyStorageAccessGrantedForOrigin(
            IPC::Principal(topLevelStoragePrincipal),
            IPC::Principal(trackingPrincipal), trackingOrigin, aAllowMode)
        ->Then(GetCurrentThreadSerialEventTarget(), __func__,
               [](const ContentChild::
                      FirstPartyStorageAccessGrantedForOriginPromise::
                          ResolveOrRejectValue& aValue) {
                 if (aValue.IsResolve()) {
                   return StorageAccessGrantPromise::CreateAndResolve(
                       aValue.ResolveValue(), __func__);
                 }
                 return StorageAccessGrantPromise::CreateAndReject(false,
                                                                   __func__);
               });
  };

  if (aPerformFinalChecks) {
    return aPerformFinalChecks()->Then(
        GetCurrentThreadSerialEventTarget(), __func__,
        [storePermission](
            StorageAccessGrantPromise::ResolveOrRejectValue&& aValue) {
          if (aValue.IsResolve()) {
            return storePermission(aValue.ResolveValue());
          }
          return StorageAccessGrantPromise::CreateAndReject(false, __func__);
        });
  }
  return storePermission(false);
}

/* static */
RefPtr<mozilla::ContentBlocking::ParentAccessGrantPromise>
ContentBlocking::SaveAccessForOriginOnParentProcess(
    nsIPrincipal* aParentPrincipal, nsIPrincipal* aTrackingPrincipal,
    const nsCString& aTrackingOrigin, int aAllowMode,
    uint64_t aExpirationTime) {
  MOZ_ASSERT(XRE_IsParentProcess());
  MOZ_ASSERT(aAllowMode == eAllow || aAllowMode == eAllowAutoGrant);

  if (!aParentPrincipal || !aTrackingPrincipal) {
    LOG(("Invalid input arguments passed"));
    return ParentAccessGrantPromise::CreateAndReject(false, __func__);
  };

  LOG_PRIN(("Saving a first-party storage permission on %s for "
            "trackingOrigin=%s",
            _spec, aTrackingOrigin.get()),
           aParentPrincipal);

  if (NS_WARN_IF(!aParentPrincipal)) {
    // The child process is sending something wrong. Let's ignore it.
    LOG(("aParentPrincipal is null, bailing out early"));
    return ParentAccessGrantPromise::CreateAndReject(false, __func__);
  }

  nsPermissionManager* permManager = nsPermissionManager::GetInstance();
  if (NS_WARN_IF(!permManager)) {
    LOG(("Permission manager is null, bailing out early"));
    return ParentAccessGrantPromise::CreateAndReject(false, __func__);
  }

  // Remember that this pref is stored in seconds!
  uint32_t expirationType = nsIPermissionManager::EXPIRE_TIME;
  uint32_t expirationTime = aExpirationTime * 1000;
  int64_t when = (PR_Now() / PR_USEC_PER_MSEC) + expirationTime;

  uint32_t privateBrowsingId = 0;
  nsresult rv = aParentPrincipal->GetPrivateBrowsingId(&privateBrowsingId);
  if ((!NS_WARN_IF(NS_FAILED(rv)) && privateBrowsingId > 0) ||
      (aAllowMode == eAllowAutoGrant)) {
    // If we are coming from a private window or are automatically granting a
    // permission, make sure to store a session-only permission which won't
    // get persisted to disk.
    expirationType = nsIPermissionManager::EXPIRE_SESSION;
    when = 0;
  }

  nsAutoCString type;
  AntiTrackingUtils::CreateStoragePermissionKey(aTrackingOrigin, type);

  LOG(
      ("Computed permission key: %s, expiry: %u, proceeding to save in the "
       "permission manager",
       type.get(), expirationTime));

  rv = permManager->AddFromPrincipal(aParentPrincipal, type,
                                     nsIPermissionManager::ALLOW_ACTION,
                                     expirationType, when);
  Unused << NS_WARN_IF(NS_FAILED(rv));

  if (NS_SUCCEEDED(rv) && (aAllowMode == eAllowAutoGrant)) {
    // Make sure temporary access grants do not survive more than 24 hours.
    TemporaryAccessGrantObserver::Create(permManager, aParentPrincipal, type);
  }

  LOG(("Result: %s", NS_SUCCEEDED(rv) ? "success" : "failure"));
  return ParentAccessGrantPromise::CreateAndResolve(rv, __func__);
}

bool ContentBlocking::ShouldAllowAccessFor(nsPIDOMWindowInner* aWindow,
                                           nsIURI* aURI,
                                           uint32_t* aRejectedReason) {
  MOZ_ASSERT(aWindow);
  MOZ_ASSERT(aURI);

  // Let's avoid a null check on aRejectedReason everywhere else.
  uint32_t rejectedReason = 0;
  if (!aRejectedReason) {
    aRejectedReason = &rejectedReason;
  }

  LOG_SPEC(("Computing whether window %p has access to URI %s", aWindow, _spec),
           aURI);

  nsGlobalWindowInner* innerWindow = nsGlobalWindowInner::Cast(aWindow);
  Document* document = innerWindow->GetExtantDoc();
  if (!document) {
    LOG(("Our window has no document"));
    return false;
  }

  BrowsingContext* topBC = aWindow->GetBrowsingContext()->Top();
  nsGlobalWindowOuter* topWindow = nullptr;
  if (topBC->IsInProcess()) {
    topWindow = nsGlobalWindowOuter::Cast(topBC->GetDOMWindow());
  } else {
    // For out-of-process top frames, we need to be able to access three things
    // from the top BrowsingContext in order to be able to port this code to
    // Fission successfully:
    //   * The CookieJarSettings of the top BrowsingContext.
    //   * The HasStorageAccessGranted() API on BrowsingContext.
    // For now, if we face an out-of-process top frame, instead of failing here,
    // we revert back to looking at the in-process top frame.  This is of course
    // the wrong thing to do, but we seem to have a number of tests in the tree
    // which are depending on this incorrect behaviour.  This path is intended
    // to temporarily keep those tests working...
    nsGlobalWindowOuter* outerWindow =
        nsGlobalWindowOuter::Cast(aWindow->GetOuterWindow());
    if (!outerWindow) {
      LOG(("Our window has no outer window"));
      return false;
    }

    nsCOMPtr<nsPIDOMWindowOuter> topOuterWindow =
        outerWindow->GetInProcessTop();
    topWindow = nsGlobalWindowOuter::Cast(topOuterWindow);
  }

  if (NS_WARN_IF(!topWindow)) {
    LOG(("No top outer window"));
    return false;
  }

  nsPIDOMWindowInner* topInnerWindow = topWindow->GetCurrentInnerWindow();
  if (NS_WARN_IF(!topInnerWindow)) {
    LOG(("No top inner window."));
    return false;
  }

  uint32_t cookiePermission = CheckCookiePermissionForPrincipal(
      document->CookieJarSettings(), document->NodePrincipal());
  if (cookiePermission != nsICookiePermission::ACCESS_DEFAULT) {
    LOG(
        ("CheckCookiePermissionForPrincipal() returned a non-default access "
         "code (%d) for window's principal, returning %s",
         int(cookiePermission),
         cookiePermission != nsICookiePermission::ACCESS_DENY ? "success"
                                                              : "failure"));
    if (cookiePermission != nsICookiePermission::ACCESS_DENY) {
      return true;
    }

    *aRejectedReason =
        nsIWebProgressListener::STATE_COOKIES_BLOCKED_BY_PERMISSION;
    return false;
  }

  int32_t behavior = CookiesBehavior(document);
  if (behavior == nsICookieService::BEHAVIOR_ACCEPT) {
    LOG(("The cookie behavior pref mandates accepting all cookies!"));
    return true;
  }

  if (ContentBlockingAllowList::Check(aWindow)) {
    return true;
  }

  if (behavior == nsICookieService::BEHAVIOR_REJECT) {
    LOG(("The cookie behavior pref mandates rejecting all cookies!"));
    *aRejectedReason = nsIWebProgressListener::STATE_COOKIES_BLOCKED_ALL;
    return false;
  }

  // As a performance optimization, we only perform this check for
  // BEHAVIOR_REJECT_FOREIGN and BEHAVIOR_LIMIT_FOREIGN.  For
  // BEHAVIOR_REJECT_TRACKER and BEHAVIOR_REJECT_TRACKER_AND_PARTITION_FOREIGN,
  // third-partiness is implicily checked later below.
  if (behavior != nsICookieService::BEHAVIOR_REJECT_TRACKER &&
      behavior !=
          nsICookieService::BEHAVIOR_REJECT_TRACKER_AND_PARTITION_FOREIGN) {
    // Let's check if this is a 3rd party context.
    if (!nsContentUtils::IsThirdPartyWindowOrChannel(aWindow, nullptr, aURI)) {
      LOG(("Our window isn't a third-party window"));
      return true;
    }
  }

  if (behavior == nsICookieService::BEHAVIOR_REJECT_FOREIGN ||
      behavior == nsICookieService::BEHAVIOR_LIMIT_FOREIGN) {
    // XXX For non-cookie forms of storage, we handle BEHAVIOR_LIMIT_FOREIGN by
    // simply rejecting the request to use the storage. In the future, if we
    // change the meaning of BEHAVIOR_LIMIT_FOREIGN to be one which makes sense
    // for non-cookie storage types, this may change.
    LOG(("Nothing more to do due to the behavior code %d", int(behavior)));
    *aRejectedReason = nsIWebProgressListener::STATE_COOKIES_BLOCKED_FOREIGN;
    return false;
  }

  MOZ_ASSERT(
      behavior == nsICookieService::BEHAVIOR_REJECT_TRACKER ||
      behavior ==
          nsICookieService::BEHAVIOR_REJECT_TRACKER_AND_PARTITION_FOREIGN);

  uint32_t blockedReason =
      nsIWebProgressListener::STATE_COOKIES_BLOCKED_TRACKER;

  if (behavior == nsICookieService::BEHAVIOR_REJECT_TRACKER) {
    if (!nsContentUtils::IsThirdPartyTrackingResourceWindow(aWindow)) {
      LOG(("Our window isn't a third-party tracking window"));
      return true;
    }

    nsCOMPtr<nsIClassifiedChannel> classifiedChannel =
        do_QueryInterface(document->GetChannel());
    if (classifiedChannel) {
      uint32_t classificationFlags =
          classifiedChannel->GetThirdPartyClassificationFlags();
      if (classificationFlags & nsIClassifiedChannel::ClassificationFlags::
                                    CLASSIFIED_SOCIALTRACKING) {
        blockedReason =
            nsIWebProgressListener::STATE_COOKIES_BLOCKED_SOCIALTRACKER;
      }
    }
  } else {
    MOZ_ASSERT(behavior ==
               nsICookieService::BEHAVIOR_REJECT_TRACKER_AND_PARTITION_FOREIGN);
    if (nsContentUtils::IsThirdPartyTrackingResourceWindow(aWindow)) {
      // fall through
    } else if (nsContentUtils::IsThirdPartyWindowOrChannel(aWindow, nullptr,
                                                           aURI)) {
      LOG(("We're in the third-party context, storage should be partitioned"));
      // fall through, but remember that we're partitioning.
      blockedReason = nsIWebProgressListener::STATE_COOKIES_PARTITIONED_FOREIGN;
    } else {
      LOG(("Our window isn't a third-party window, storage is allowed"));
      return true;
    }
  }

#ifdef DEBUG
  nsCOMPtr<mozIThirdPartyUtil> thirdPartyUtil = services::GetThirdPartyUtil();
  if (thirdPartyUtil) {
    bool thirdParty = false;
    nsresult rv = thirdPartyUtil->IsThirdPartyWindow(aWindow->GetOuterWindow(),
                                                     aURI, &thirdParty);
    // The result of this assertion depends on whether IsThirdPartyWindow
    // succeeds, because otherwise IsThirdPartyWindowOrChannel artificially
    // fails.
    MOZ_ASSERT_IF(NS_SUCCEEDED(rv), nsContentUtils::IsThirdPartyWindowOrChannel(
                                        aWindow, nullptr, aURI) == thirdParty);
  }
#endif

  Document* doc = aWindow->GetExtantDoc();
  // Make sure storage access isn't disabled
  if (doc && (doc->StorageAccessSandboxed())) {
    LOG(("Our document is sandboxed"));
    return false;
  }

  nsCOMPtr<nsIPrincipal> parentPrincipal;
  nsAutoCString trackingOrigin;
  if (!GetParentPrincipalAndTrackingOrigin(
          nsGlobalWindowInner::Cast(aWindow), behavior,
          getter_AddRefs(parentPrincipal), trackingOrigin, nullptr)) {
    LOG(("Failed to obtain the parent principal and the tracking origin"));
    *aRejectedReason = blockedReason;
    return false;
  }

  nsAutoCString type;
  AntiTrackingUtils::CreateStoragePermissionKey(trackingOrigin, type);

  if (topInnerWindow->HasStorageAccessGranted(type)) {
    LOG(("Permission stored in the window. All good."));
    return true;
  }

  return CheckAntiTrackingPermission(
      parentPrincipal, type, nsContentUtils::IsInPrivateBrowsing(document),
      aRejectedReason, blockedReason);
}

bool ContentBlocking::ShouldAllowAccessFor(nsIChannel* aChannel, nsIURI* aURI,
                                           uint32_t* aRejectedReason) {
  MOZ_ASSERT(aURI);
  MOZ_ASSERT(aChannel);

  // Let's avoid a null check on aRejectedReason everywhere else.
  uint32_t rejectedReason = 0;
  if (!aRejectedReason) {
    aRejectedReason = &rejectedReason;
  }

  nsIScriptSecurityManager* ssm =
      nsScriptSecurityManager::GetScriptSecurityManager();
  MOZ_ASSERT(ssm);

  nsCOMPtr<nsIURI> channelURI;
  nsresult rv = NS_GetFinalChannelURI(aChannel, getter_AddRefs(channelURI));
  if (NS_FAILED(rv)) {
    LOG(("Failed to get the channel final URI, bail out early"));
    return true;
  }
  LOG_SPEC(
      ("Computing whether channel %p has access to URI %s", aChannel, _spec),
      channelURI);

  nsCOMPtr<nsILoadInfo> loadInfo = aChannel->LoadInfo();
  // We need to find the correct principal to check the cookie permission. For
  // third-party contexts, we want to check if the top-level window has a custom
  // cookie permission.
  nsCOMPtr<nsIPrincipal> toplevelPrincipal = loadInfo->GetTopLevelPrincipal();

  // If this is already the top-level window, we should use the loading
  // principal.
  if (!toplevelPrincipal) {
    LOG(
        ("Our loadInfo lacks a top-level principal, use the loadInfo's loading "
         "principal instead"));
    toplevelPrincipal = loadInfo->LoadingPrincipal();
  }

  nsCOMPtr<nsIHttpChannel> httpChannel = do_QueryInterface(aChannel);

  // If we don't have a loading principal and this is a document channel, we are
  // a top-level window!
  if (!toplevelPrincipal) {
    LOG(
        ("We don't have a loading principal, let's see if this is a document "
         "channel"
         " that belongs to a top-level window"));
    bool isDocument = false;
    if (httpChannel) {
      rv = httpChannel->GetIsMainDocumentChannel(&isDocument);
    }
    if (httpChannel && NS_SUCCEEDED(rv) && isDocument) {
      rv = ssm->GetChannelResultPrincipal(aChannel,
                                          getter_AddRefs(toplevelPrincipal));
      if (NS_SUCCEEDED(rv)) {
        LOG(("Yes, we guessed right!"));
      } else {
        LOG(
            ("Yes, we guessed right, but minting the channel result principal "
             "failed"));
      }
    } else {
      LOG(("No, we guessed wrong!"));
    }
  }

  // Let's use the triggering principal then.
  if (!toplevelPrincipal) {
    LOG(
        ("Our loadInfo lacks a top-level principal, use the loadInfo's "
         "triggering principal instead"));
    toplevelPrincipal = loadInfo->TriggeringPrincipal();
  }

  if (NS_WARN_IF(!toplevelPrincipal)) {
    LOG(("No top-level principal! Bail out early"));
    return false;
  }

  nsCOMPtr<nsICookieJarSettings> cookieJarSettings;
  rv = loadInfo->GetCookieJarSettings(getter_AddRefs(cookieJarSettings));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    LOG(
        ("Failed to get the cookie jar settings from the loadinfo, bail out "
         "early"));
    return true;
  }

  nsCOMPtr<nsIPrincipal> channelPrincipal;
  rv = ssm->GetChannelURIPrincipal(aChannel, getter_AddRefs(channelPrincipal));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    LOG(("No channel principal, bail out early"));
    return false;
  }

  uint32_t cookiePermission =
      CheckCookiePermissionForPrincipal(cookieJarSettings, channelPrincipal);
  if (cookiePermission != nsICookiePermission::ACCESS_DEFAULT) {
    LOG(
        ("CheckCookiePermissionForPrincipal() returned a non-default access "
         "code (%d) for channel's principal, returning %s",
         int(cookiePermission),
         cookiePermission != nsICookiePermission::ACCESS_DENY ? "success"
                                                              : "failure"));
    if (cookiePermission != nsICookiePermission::ACCESS_DENY) {
      return true;
    }

    *aRejectedReason =
        nsIWebProgressListener::STATE_COOKIES_BLOCKED_BY_PERMISSION;
    return false;
  }

  if (!channelURI) {
    LOG(("No channel uri, bail out early"));
    return false;
  }

  int32_t behavior = CookiesBehavior(loadInfo, channelURI);
  if (behavior == nsICookieService::BEHAVIOR_ACCEPT) {
    LOG(("The cookie behavior pref mandates accepting all cookies!"));
    return true;
  }

  if (httpChannel && ContentBlockingAllowList::Check(httpChannel)) {
    return true;
  }

  if (behavior == nsICookieService::BEHAVIOR_REJECT) {
    LOG(("The cookie behavior pref mandates rejecting all cookies!"));
    *aRejectedReason = nsIWebProgressListener::STATE_COOKIES_BLOCKED_ALL;
    return false;
  }

  nsCOMPtr<mozIThirdPartyUtil> thirdPartyUtil = services::GetThirdPartyUtil();
  if (!thirdPartyUtil) {
    LOG(("No thirdPartyUtil, bail out early"));
    return true;
  }

  bool thirdParty = false;
  rv = thirdPartyUtil->IsThirdPartyChannel(aChannel, aURI, &thirdParty);
  // Grant if it's not a 3rd party.
  // Be careful to check the return value of IsThirdPartyChannel, since
  // IsThirdPartyChannel() will fail if the channel's loading principal is the
  // system principal...
  if (NS_SUCCEEDED(rv) && !thirdParty) {
    LOG(("Our channel isn't a third-party channel"));
    return true;
  }

  if (behavior == nsICookieService::BEHAVIOR_REJECT_FOREIGN ||
      behavior == nsICookieService::BEHAVIOR_LIMIT_FOREIGN) {
    // XXX For non-cookie forms of storage, we handle BEHAVIOR_LIMIT_FOREIGN by
    // simply rejecting the request to use the storage. In the future, if we
    // change the meaning of BEHAVIOR_LIMIT_FOREIGN to be one which makes sense
    // for non-cookie storage types, this may change.
    LOG(("Nothing more to do due to the behavior code %d", int(behavior)));
    *aRejectedReason = nsIWebProgressListener::STATE_COOKIES_BLOCKED_FOREIGN;
    return false;
  }

  MOZ_ASSERT(
      behavior == nsICookieService::BEHAVIOR_REJECT_TRACKER ||
      behavior ==
          nsICookieService::BEHAVIOR_REJECT_TRACKER_AND_PARTITION_FOREIGN);

  uint32_t blockedReason =
      nsIWebProgressListener::STATE_COOKIES_BLOCKED_TRACKER;

  // Not a tracker.
  nsCOMPtr<nsIClassifiedChannel> classifiedChannel =
      do_QueryInterface(aChannel);
  if (behavior == nsICookieService::BEHAVIOR_REJECT_TRACKER) {
    if (classifiedChannel) {
      if (!classifiedChannel->IsThirdPartyTrackingResource()) {
        LOG(("Our channel isn't a third-party tracking channel"));
        return true;
      }

      uint32_t classificationFlags =
          classifiedChannel->GetThirdPartyClassificationFlags();
      if (classificationFlags & nsIClassifiedChannel::ClassificationFlags::
                                    CLASSIFIED_SOCIALTRACKING) {
        blockedReason =
            nsIWebProgressListener::STATE_COOKIES_BLOCKED_SOCIALTRACKER;
      }
    }
  } else {
    MOZ_ASSERT(behavior ==
               nsICookieService::BEHAVIOR_REJECT_TRACKER_AND_PARTITION_FOREIGN);
    if (classifiedChannel &&
        classifiedChannel->IsThirdPartyTrackingResource()) {
      // fall through
    } else if (nsContentUtils::IsThirdPartyWindowOrChannel(nullptr, aChannel,
                                                           aURI)) {
      LOG(("We're in the third-party context, storage should be partitioned"));
      // fall through but remember that we're partitioning.
      blockedReason = nsIWebProgressListener::STATE_COOKIES_PARTITIONED_FOREIGN;
    } else {
      LOG(("Our channel isn't a third-party channel, storage is allowed"));
      return true;
    }
  }

  // Only use the "top-level storage area principal" behaviour for reject
  // tracker mode only.
  nsIPrincipal* parentPrincipal =
      (behavior == nsICookieService::BEHAVIOR_REJECT_TRACKER)
          ? loadInfo->GetTopLevelStorageAreaPrincipal()
          : loadInfo->GetTopLevelPrincipal();
  if (!parentPrincipal) {
    LOG(("No top-level storage area principal at hand"));

    // parentPrincipal can be null if the parent window is not the top-level
    // window.
    if (loadInfo->GetTopLevelPrincipal()) {
      LOG(("Parent window is the top-level window, bail out early"));
      *aRejectedReason = blockedReason;
      return false;
    }

    parentPrincipal = toplevelPrincipal;
    if (NS_WARN_IF(!parentPrincipal)) {
      LOG(
          ("No triggering principal, this shouldn't be happening! Bail out "
           "early"));
      // Why we are here?!?
      return true;
    }
  }

  // Let's see if we have to grant the access for this particular channel.

  nsCOMPtr<nsIURI> trackingURI;
  rv = aChannel->GetURI(getter_AddRefs(trackingURI));
  if (NS_WARN_IF(NS_FAILED(rv))) {
    LOG(("Failed to get the channel URI"));
    return true;
  }

  nsAutoCString trackingOrigin;
  rv = nsContentUtils::GetASCIIOrigin(trackingURI, trackingOrigin);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    LOG_SPEC(("Failed to compute the origin from %s", _spec), trackingURI);
    return false;
  }

  nsAutoCString type;
  AntiTrackingUtils::CreateStoragePermissionKey(trackingOrigin, type);

  uint32_t privateBrowsingId = 0;
  rv = channelPrincipal->GetPrivateBrowsingId(&privateBrowsingId);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    LOG(("Failed to get the channel principal's private browsing ID"));
    return false;
  }

  return CheckAntiTrackingPermission(parentPrincipal, type, !!privateBrowsingId,
                                     aRejectedReason, blockedReason);
}

bool ContentBlocking::ShouldAllowAccessFor(
    nsIPrincipal* aPrincipal, nsICookieJarSettings* aCookieJarSettings) {
  MOZ_ASSERT(aPrincipal);
  MOZ_ASSERT(aCookieJarSettings);

  uint32_t access = nsICookiePermission::ACCESS_DEFAULT;
  if (aPrincipal->GetIsContentPrincipal()) {
    nsPermissionManager* permManager = nsPermissionManager::GetInstance();
    if (permManager) {
      Unused << NS_WARN_IF(NS_FAILED(permManager->TestPermissionFromPrincipal(
          aPrincipal, NS_LITERAL_CSTRING("cookie"), &access)));
    }
  }

  if (access != nsICookiePermission::ACCESS_DEFAULT) {
    return access != nsICookiePermission::ACCESS_DENY;
  }

  int32_t behavior = CookiesBehavior(aPrincipal, aCookieJarSettings);
  return behavior != nsICookieService::BEHAVIOR_REJECT;
}

/* static */
bool ContentBlocking::ApproximateAllowAccessForWithoutChannel(
    nsPIDOMWindowInner* aFirstPartyWindow, nsIURI* aURI) {
  MOZ_ASSERT(aFirstPartyWindow);
  MOZ_ASSERT(aURI);

  LOG_SPEC(
      ("Computing a best guess as to whether window %p has access to URI %s",
       aFirstPartyWindow, _spec),
      aURI);

  Document* parentDocument =
      nsGlobalWindowInner::Cast(aFirstPartyWindow)->GetExtantDoc();
  if (NS_WARN_IF(!parentDocument)) {
    LOG(("Failed to get the first party window's document"));
    return false;
  }

  if (!parentDocument->CookieJarSettings()->GetRejectThirdPartyTrackers()) {
    LOG(("Disabled by the pref (%d), bail out early",
         parentDocument->CookieJarSettings()->GetCookieBehavior()));
    return true;
  }

  if (ContentBlockingAllowList::Check(aFirstPartyWindow)) {
    return true;
  }

  if (!nsContentUtils::IsThirdPartyWindowOrChannel(aFirstPartyWindow, nullptr,
                                                   aURI)) {
    LOG(("Our window isn't a third-party window"));
    return true;
  }

  uint32_t cookiePermission = CheckCookiePermissionForPrincipal(
      parentDocument->CookieJarSettings(), parentDocument->NodePrincipal());
  if (cookiePermission != nsICookiePermission::ACCESS_DEFAULT) {
    LOG(
        ("CheckCookiePermissionForPrincipal() returned a non-default access "
         "code (%d), returning %s",
         int(cookiePermission),
         cookiePermission != nsICookiePermission::ACCESS_DENY ? "success"
                                                              : "failure"));
    return cookiePermission != nsICookiePermission::ACCESS_DENY;
  }

  nsAutoCString origin;
  nsresult rv = nsContentUtils::GetASCIIOrigin(aURI, origin);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    LOG_SPEC(("Failed to compute the origin from %s", _spec), aURI);
    return false;
  }

  nsIPrincipal* parentPrincipal = parentDocument->NodePrincipal();

  nsAutoCString type;
  AntiTrackingUtils::CreateStoragePermissionKey(origin, type);

  return CheckAntiTrackingPermission(
      parentPrincipal, type,
      nsContentUtils::IsInPrivateBrowsing(parentDocument), nullptr, 0);
}
