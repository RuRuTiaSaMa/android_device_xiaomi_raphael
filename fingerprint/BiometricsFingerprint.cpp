/*
 * Copyright (C) 2017 The Android Open Source Project
 * Copyright (C) 2022 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "android.hardware.biometrics.fingerprint@2.3-service.xiaomi_raphael"

#include <hardware/hw_auth_token.h>

#include <hardware/fingerprint.h>
#include <hardware/hardware.h>
#include "BiometricsFingerprint.h"
#include "xiaomi_fingerprint.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <unistd.h>
#include <thread>

#include <android/binder_manager.h>
#include <aidl/android/hardware/power/IPower.h>
#include <aidl/google/hardware/power/extension/pixel/IPowerExt.h>

using ::aidl::android::hardware::power::IPower;
using ::aidl::google::hardware::power::extension::pixel::IPowerExt;

namespace android {
namespace hardware {
namespace biometrics {
namespace fingerprint {
namespace V2_3 {
namespace implementation {

#define COMMAND_NIT 10
#define PARAM_NIT_FOD 1
#define PARAM_NIT_NONE 0

#define FOD_UI_PATH "/sys/devices/platform/soc/soc:qcom,dsi-display-primary/fod_ui"

static bool readBool(int fd) {
    char c;
    int rc;

    rc = lseek(fd, 0, SEEK_SET);
    if (rc) {
        LOG(ERROR) << "failed to seek fd, err: " << rc;
        return false;
    }

    rc = read(fd, &c, sizeof(char));
    if (rc != 1) {
        LOG(ERROR) << "failed to read bool from fd, err: " << rc;
        return false;
    }

    return c != '0';
}

// Supported fingerprint HAL version
static const uint16_t kVersion = HARDWARE_MODULE_API_VERSION(2, 1);

constexpr char kBoostHint[] = "LAUNCH";
constexpr int32_t kBoostDurationMs = 2000;

typedef struct fingerprint_hal {
    const char* class_name;
    const bool fod;
} fingerprint_hal_t;

static const fingerprint_hal_t kModules[] = {
        {"fpc", false},        {"fpc_fod", true}, {"goodix", false}, {"goodix_fod", true},
        {"goodix_fod6", true}, {"silead", false}, {"syna", true},
};

using RequestStatus = android::hardware::biometrics::fingerprint::V2_1::RequestStatus;

using ::android::base::SetProperty;

BiometricsFingerprint* BiometricsFingerprint::sInstance = nullptr;

BiometricsFingerprint::BiometricsFingerprint() : mClientCallback(nullptr), mDevice(nullptr), mBoostHintIsSupported(false), mBoostHintSupportIsChecked(false), mPowerHalExtAidl(nullptr) {
    sInstance = this;  // keep track of the most recent instance
    for (auto& [class_name, fod] : kModules) {
        mDevice = openHal(class_name);
        if (!mDevice) {
            LOG(ERROR) << "Can't open HAL module, class " << class_name;
            continue;
        }

        LOG(INFO) << "Opened fingerprint HAL, class " << class_name;
        mFod = fod;
        SetProperty("persist.vendor.sys.fp.vendor", class_name);
        break;
    }
    if (!mDevice) {
        LOG(ERROR) << "Can't open any HAL module";
    }

    if (mFod) {
        std::thread([this]() {
            int fd = open(FOD_UI_PATH, O_RDONLY);
            if (fd < 0) {
                LOG(ERROR) << "failed to open fd, err: " << fd;
                return;
            }

            struct pollfd fodUiPoll = {
                    .fd = fd,
                    .events = POLLERR | POLLPRI,
                    .revents = 0,
            };

            while (true) {
                int rc = poll(&fodUiPoll, 1, -1);
                if (rc < 0) {
                    LOG(ERROR) << "failed to poll fd, err: " << rc;
                    continue;
                }

                extCmd(COMMAND_NIT, readBool(fd) ? PARAM_NIT_FOD : PARAM_NIT_NONE);
            }
        }).detach();

        SetProperty("ro.hardware.fp.fod", "true");
    }
}

int32_t BiometricsFingerprint::connectPowerHalExt() {
    if (mPowerHalExtAidl) {
        return android::NO_ERROR;
    }
    const std::string kInstance = std::string(IPower::descriptor) + "/default";
    ndk::SpAIBinder pwBinder = ndk::SpAIBinder(AServiceManager_getService(kInstance.c_str()));
    ndk::SpAIBinder pwExtBinder;
    AIBinder_getExtension(pwBinder.get(), pwExtBinder.getR());
    mPowerHalExtAidl = IPowerExt::fromBinder(pwExtBinder);
    if (!mPowerHalExtAidl) {
        LOG(ERROR) << "failed to connect power HAL extension";
        return -EINVAL;
    }
    LOG(INFO) << "connect power HAL extension successfully";
    return android::NO_ERROR;
}

int32_t BiometricsFingerprint::checkPowerHalExtBoostSupport(const std::string &boost) {
    if (boost.empty() || connectPowerHalExt() != android::NO_ERROR) {
        return -EINVAL;
    }
    bool isSupported = false;
    auto ret = mPowerHalExtAidl->isBoostSupported(boost.c_str(), &isSupported);
    if (!ret.isOk()) {
        LOG(ERROR) << "failed to check power HAL extension hint: boost=" << boost.c_str();
        if (ret.getExceptionCode() == EX_TRANSACTION_FAILED) {
            /*
             * PowerHAL service may crash due to some reasons, this could end up
             * binder transaction failure. Set nullptr here to trigger re-connection.
             */
            LOG(ERROR) << "binder transaction failed for power HAL extension hint";
            mPowerHalExtAidl = nullptr;
            return -ENOTCONN;
        }
        return -EINVAL;
    }
    if (!isSupported) {
        LOG(WARNING) << "power HAL extension hint is not supported: boost=" << boost.c_str();
        return -EOPNOTSUPP;
    }
    LOG(INFO) << "power HAL extension hint is supported: boost=" << boost.c_str();
    return android::NO_ERROR;
}

int32_t BiometricsFingerprint::sendPowerHalExtBoost(const std::string &boost,
                                                               int32_t durationMs) {
    if (boost.empty() || connectPowerHalExt() != android::NO_ERROR) {
        return -EINVAL;
    }
    auto ret = mPowerHalExtAidl->setBoost(boost.c_str(), durationMs);
    if (!ret.isOk()) {
        LOG(ERROR) << "failed to send power HAL extension hint: boost=" << boost.c_str() << ", duration=" << durationMs;
        if (ret.getExceptionCode() == EX_TRANSACTION_FAILED) {
            /*
             * PowerHAL service may crash due to some reasons, this could end up
             * binder transaction failure. Set nullptr here to trigger re-connection.
             */
            LOG(ERROR) << "binder transaction failed for power HAL extension hint";
            mPowerHalExtAidl = nullptr;
            return -ENOTCONN;
        }
        return -EINVAL;
    }
    return android::NO_ERROR;
}

int32_t BiometricsFingerprint::isBoostHintSupported() {
    int32_t ret = android::NO_ERROR;
    if (mBoostHintSupportIsChecked) {
        ret = mBoostHintIsSupported ? android::NO_ERROR : -EOPNOTSUPP;
        return ret;
    }
    ret = checkPowerHalExtBoostSupport(kBoostHint);
    if (ret == android::NO_ERROR) {
        mBoostHintIsSupported = true;
        mBoostHintSupportIsChecked = true;
        LOG(INFO) << "Boost hint is supported";
    } else if (ret == -EOPNOTSUPP) {
        mBoostHintSupportIsChecked = true;
        LOG(INFO) << "Boost hint is unsupported";
    } else {
        LOG(ERROR) << "Failed to check the support of boost hint, ret " << ret;
    }
    return ret;
}

int32_t BiometricsFingerprint::sendAuthenticatedBoostHint() {
    int32_t ret = isBoostHintSupported();
    if (ret != android::NO_ERROR) {
        return ret;
    }
    ret = sendPowerHalExtBoost(kBoostHint, kBoostDurationMs);
    return ret;
}

BiometricsFingerprint::~BiometricsFingerprint() {
    LOG(VERBOSE) << "~BiometricsFingerprint()";
    if (mDevice == nullptr) {
        LOG(ERROR) << "No valid device";
        return;
    }
    int err;
    if (0 != (err = mDevice->common.close(reinterpret_cast<hw_device_t*>(mDevice)))) {
        LOG(ERROR) << "Can't close fingerprint module, error: " << err;
        return;
    }
    mDevice = nullptr;
}

Return<RequestStatus> BiometricsFingerprint::ErrorFilter(int32_t error) {
    switch (error) {
        case 0:
            return RequestStatus::SYS_OK;
        case -2:
            return RequestStatus::SYS_ENOENT;
        case -4:
            return RequestStatus::SYS_EINTR;
        case -5:
            return RequestStatus::SYS_EIO;
        case -11:
            return RequestStatus::SYS_EAGAIN;
        case -12:
            return RequestStatus::SYS_ENOMEM;
        case -13:
            return RequestStatus::SYS_EACCES;
        case -14:
            return RequestStatus::SYS_EFAULT;
        case -16:
            return RequestStatus::SYS_EBUSY;
        case -22:
            return RequestStatus::SYS_EINVAL;
        case -28:
            return RequestStatus::SYS_ENOSPC;
        case -110:
            return RequestStatus::SYS_ETIMEDOUT;
        default:
            LOG(ERROR) << "An unknown error returned from fingerprint vendor library: " << error;
            return RequestStatus::SYS_UNKNOWN;
    }
}

// Translate from errors returned by traditional HAL (see fingerprint.h) to
// HIDL-compliant FingerprintError.
FingerprintError BiometricsFingerprint::VendorErrorFilter(int32_t error, int32_t* vendorCode) {
    *vendorCode = 0;
    switch (error) {
        case FINGERPRINT_ERROR_HW_UNAVAILABLE:
            return FingerprintError::ERROR_HW_UNAVAILABLE;
        case FINGERPRINT_ERROR_UNABLE_TO_PROCESS:
            return FingerprintError::ERROR_UNABLE_TO_PROCESS;
        case FINGERPRINT_ERROR_TIMEOUT:
            return FingerprintError::ERROR_TIMEOUT;
        case FINGERPRINT_ERROR_NO_SPACE:
            return FingerprintError::ERROR_NO_SPACE;
        case FINGERPRINT_ERROR_CANCELED:
            return FingerprintError::ERROR_CANCELED;
        case FINGERPRINT_ERROR_UNABLE_TO_REMOVE:
            return FingerprintError::ERROR_UNABLE_TO_REMOVE;
        case FINGERPRINT_ERROR_LOCKOUT:
            return FingerprintError::ERROR_LOCKOUT;
        default:
            if (error >= FINGERPRINT_ERROR_VENDOR_BASE) {
                // vendor specific code.
                *vendorCode = error - FINGERPRINT_ERROR_VENDOR_BASE;
                return FingerprintError::ERROR_VENDOR;
            }
    }
    LOG(ERROR) << "Unknown error from fingerprint vendor library: " << error;
    return FingerprintError::ERROR_UNABLE_TO_PROCESS;
}

// Translate acquired messages returned by traditional HAL (see fingerprint.h)
// to HIDL-compliant FingerprintAcquiredInfo.
FingerprintAcquiredInfo BiometricsFingerprint::VendorAcquiredFilter(int32_t info,
                                                                    int32_t* vendorCode) {
    *vendorCode = 0;
    switch (info) {
        case FINGERPRINT_ACQUIRED_GOOD:
            return FingerprintAcquiredInfo::ACQUIRED_GOOD;
        case FINGERPRINT_ACQUIRED_PARTIAL:
            return FingerprintAcquiredInfo::ACQUIRED_PARTIAL;
        case FINGERPRINT_ACQUIRED_INSUFFICIENT:
            return FingerprintAcquiredInfo::ACQUIRED_INSUFFICIENT;
        case FINGERPRINT_ACQUIRED_IMAGER_DIRTY:
            return FingerprintAcquiredInfo::ACQUIRED_IMAGER_DIRTY;
        case FINGERPRINT_ACQUIRED_TOO_SLOW:
            return FingerprintAcquiredInfo::ACQUIRED_TOO_SLOW;
        case FINGERPRINT_ACQUIRED_TOO_FAST:
            return FingerprintAcquiredInfo::ACQUIRED_TOO_FAST;
        default:
            if (info >= FINGERPRINT_ACQUIRED_VENDOR_BASE) {
                // vendor specific code.
                *vendorCode = info - FINGERPRINT_ACQUIRED_VENDOR_BASE;
                return FingerprintAcquiredInfo::ACQUIRED_VENDOR;
            }
    }
    LOG(ERROR) << "Unknown acquiredmsg from fingerprint vendor library: " << info;
    return FingerprintAcquiredInfo::ACQUIRED_INSUFFICIENT;
}

Return<uint64_t> BiometricsFingerprint::setNotify(
        const sp<IBiometricsFingerprintClientCallback>& clientCallback) {
    std::lock_guard<std::mutex> lock(mClientCallbackMutex);
    mClientCallback = clientCallback;
    // This is here because HAL 2.1 doesn't have a way to propagate a
    // unique token for its driver. Subsequent versions should send a unique
    // token for each call to setNotify(). This is fine as long as there's only
    // one fingerprint device on the platform.
    return reinterpret_cast<uint64_t>(mDevice);
}

Return<uint64_t> BiometricsFingerprint::preEnroll() {
    return mDevice->pre_enroll(mDevice);
}

Return<RequestStatus> BiometricsFingerprint::enroll(const hidl_array<uint8_t, 69>& hat,
                                                    uint32_t gid, uint32_t timeoutSec) {
    const hw_auth_token_t* authToken = reinterpret_cast<const hw_auth_token_t*>(hat.data());
    return ErrorFilter(mDevice->enroll(mDevice, authToken, gid, timeoutSec));
}

Return<RequestStatus> BiometricsFingerprint::postEnroll() {
    return ErrorFilter(mDevice->post_enroll(mDevice));
}

Return<uint64_t> BiometricsFingerprint::getAuthenticatorId() {
    return mDevice->get_authenticator_id(mDevice);
}

Return<RequestStatus> BiometricsFingerprint::cancel() {
    return ErrorFilter(mDevice->cancel(mDevice));
}

Return<RequestStatus> BiometricsFingerprint::enumerate() {
    return ErrorFilter(mDevice->enumerate(mDevice));
}

Return<RequestStatus> BiometricsFingerprint::remove(uint32_t gid, uint32_t fid) {
    return ErrorFilter(mDevice->remove(mDevice, gid, fid));
}

Return<RequestStatus> BiometricsFingerprint::setActiveGroup(uint32_t gid,
                                                            const hidl_string& storePath) {
    if (storePath.size() >= PATH_MAX || storePath.size() <= 0) {
        LOG(ERROR) << "Bad path length: " << storePath.size();
        return RequestStatus::SYS_EINVAL;
    }
    if (access(storePath.c_str(), W_OK)) {
        return RequestStatus::SYS_EINVAL;
    }

    return ErrorFilter(mDevice->set_active_group(mDevice, gid, storePath.c_str()));
}

Return<RequestStatus> BiometricsFingerprint::authenticate(uint64_t operationId, uint32_t gid) {
    return ErrorFilter(mDevice->authenticate(mDevice, operationId, gid));
}

Return<bool> BiometricsFingerprint::isUdfps(uint32_t /*sensorId*/) {
    return mFod;
}

Return<void> BiometricsFingerprint::onFingerDown(uint32_t /*x*/, uint32_t /*y*/, float /*minor*/,
                                                 float /*major*/) {
    return Void();
}

Return<void> BiometricsFingerprint::onFingerUp() {
    return Void();
}

Return<int32_t> BiometricsFingerprint::extCmd(int32_t cmd, int32_t param) {
    return mDevice->extCmd(mDevice, cmd, param);
}

xiaomi_fingerprint_device_t* BiometricsFingerprint::openHal(const char* class_name) {
    int err;
    const hw_module_t* hw_mdl = nullptr;
    LOG(DEBUG) << "Opening fingerprint hal library...";
    if (0 != (err = hw_get_module_by_class(FINGERPRINT_HARDWARE_MODULE_ID, class_name, &hw_mdl))) {
        LOG(ERROR) << "Can't open fingerprint HW Module, error: " << err;
        return nullptr;
    }

    if (hw_mdl == nullptr) {
        LOG(ERROR) << "No valid fingerprint module";
        return nullptr;
    }

    fingerprint_module_t const* module = reinterpret_cast<const fingerprint_module_t*>(hw_mdl);
    if (module->common.methods->open == nullptr) {
        LOG(ERROR) << "No valid open method";
        return nullptr;
    }

    hw_device_t* device = nullptr;

    if (0 != (err = module->common.methods->open(hw_mdl, nullptr, &device))) {
        LOG(ERROR) << "Can't open fingerprint methods, error: " << err;
        return nullptr;
    }

    if (kVersion != device->version) {
        // enforce version on new devices because of HIDL@2.1 translation layer
        LOG(ERROR) << "Wrong fp version. Expected " << device->version << ", got " << kVersion;
        return nullptr;
    }

    xiaomi_fingerprint_device_t* fp_device = reinterpret_cast<xiaomi_fingerprint_device_t*>(device);

    if (0 != (err = fp_device->set_notify(fp_device, BiometricsFingerprint::notify))) {
        LOG(ERROR) << "Can't register fingerprint module callback, error: " << err;
        return nullptr;
    }

    return fp_device;
}

void BiometricsFingerprint::notify(const fingerprint_msg_t* msg) {
    BiometricsFingerprint* thisPtr = BiometricsFingerprint::getInstance<BiometricsFingerprint>();
    std::lock_guard<std::mutex> lock(thisPtr->mClientCallbackMutex);
    if (thisPtr == nullptr || thisPtr->mClientCallback == nullptr) {
        LOG(ERROR) << "Receiving callbacks before the client callback is registered.";
        return;
    }
    const uint64_t devId = reinterpret_cast<uint64_t>(thisPtr->mDevice);
    switch (msg->type) {
        case FINGERPRINT_ERROR: {
            int32_t vendorCode = 0;
            FingerprintError result = VendorErrorFilter(msg->data.error, &vendorCode);
            LOG(DEBUG) << "onError(" << static_cast<int>(result) << ")";
            if (!thisPtr->mClientCallback->onError(devId, result, vendorCode).isOk()) {
                LOG(ERROR) << "failed to invoke fingerprint onError callback";
            }
        } break;
        case FINGERPRINT_ACQUIRED: {
            int32_t vendorCode = 0;
            FingerprintAcquiredInfo result =
                    VendorAcquiredFilter(msg->data.acquired.acquired_info, &vendorCode);
            LOG(DEBUG) << "onAcquired(" << static_cast<int>(result) << ")";
            if (!thisPtr->mClientCallback->onAcquired(devId, result, vendorCode).isOk()) {
                LOG(ERROR) << "failed to invoke fingerprint onAcquired callback";
            }
        } break;
        case FINGERPRINT_TEMPLATE_ENROLLING:
            LOG(DEBUG) << "onEnrollResult(fid=" << msg->data.enroll.finger.fid
                       << ", gid=" << msg->data.enroll.finger.gid
                       << ", rem=" << msg->data.enroll.samples_remaining << ")";
            if (!thisPtr->mClientCallback
                         ->onEnrollResult(devId, msg->data.enroll.finger.fid,
                                          msg->data.enroll.finger.gid,
                                          msg->data.enroll.samples_remaining)
                         .isOk()) {
                LOG(ERROR) << "failed to invoke fingerprint onEnrollResult callback";
            }
            break;
        case FINGERPRINT_TEMPLATE_REMOVED:
            LOG(DEBUG) << "onRemove(fid=" << msg->data.removed.finger.fid
                       << ", gid=" << msg->data.removed.finger.gid
                       << ", rem=" << msg->data.removed.remaining_templates << ")";
            if (!thisPtr->mClientCallback
                         ->onRemoved(devId, msg->data.removed.finger.fid,
                                     msg->data.removed.finger.gid,
                                     msg->data.removed.remaining_templates)
                         .isOk()) {
                LOG(ERROR) << "failed to invoke fingerprint onRemoved callback";
            }
            break;
        case FINGERPRINT_AUTHENTICATED:
            if (msg->data.authenticated.finger.fid != 0) {
                LOG(DEBUG) << "onAuthenticated(fid=" << msg->data.authenticated.finger.fid
                           << ", gid=" << msg->data.authenticated.finger.gid << ")";
                const uint8_t* hat = reinterpret_cast<const uint8_t*>(&msg->data.authenticated.hat);
                const hidl_vec<uint8_t> token(
                        std::vector<uint8_t>(hat, hat + sizeof(msg->data.authenticated.hat)));
                if (!thisPtr->mClientCallback
                             ->onAuthenticated(devId, msg->data.authenticated.finger.fid,
                                               msg->data.authenticated.finger.gid, token)
                             .isOk()) {
                    LOG(ERROR) << "failed to invoke fingerprint onAuthenticated callback";
                } else {
                    if (thisPtr->sendAuthenticatedBoostHint() != android::NO_ERROR) {
                        LOG(ERROR) << "failed to send authenticated boost";
                    }
                }
            } else {
                // Not a recognized fingerprint
                if (!thisPtr->mClientCallback
                             ->onAuthenticated(devId, msg->data.authenticated.finger.fid,
                                               msg->data.authenticated.finger.gid,
                                               hidl_vec<uint8_t>())
                             .isOk()) {
                    LOG(ERROR) << "failed to invoke fingerprint onAuthenticated callback";
                }
            }
            break;
        case FINGERPRINT_TEMPLATE_ENUMERATING:
            LOG(DEBUG) << "onEnumerate(fid=" << msg->data.enumerated.finger.fid
                       << ", gid=" << msg->data.enumerated.finger.gid
                       << ", rem=" << msg->data.enumerated.remaining_templates << ")";
            if (!thisPtr->mClientCallback
                         ->onEnumerate(devId, msg->data.enumerated.finger.fid,
                                       msg->data.enumerated.finger.gid,
                                       msg->data.enumerated.remaining_templates)
                         .isOk()) {
                LOG(ERROR) << "failed to invoke fingerprint onEnumerate callback";
            }
            break;
    }
}

}  // namespace implementation
}  // namespace V2_3
}  // namespace fingerprint
}  // namespace biometrics
}  // namespace hardware
}  // namespace android
