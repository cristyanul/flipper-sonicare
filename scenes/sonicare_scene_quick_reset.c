#include "../uk_mbirth_sonicare.h"
#include "../sonicare_password.h"
#include <uk_mbirth_sonicare_icons.h>

#include <gui/scene_manager.h>
#include <gui/view_dispatcher.h>
#include <nfc/nfc.h>
#include <nfc/nfc_device.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight_poller.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <dolphin/dolphin.h>

#define TAG "sonicare_quick_reset"
#define SONICARE_PAGE_USAGE 0x24

static bool sonicare_quick_reset_validate_mfg(const uint8_t* mfg) {
    for(int i = 0; i < 6; i++) {
        if(mfg[i] < '0' || mfg[i] > '9') return false;
    }
    if(mfg[6] != ' ') return false;
    for(int i = 7; i < 10; i++) {
        if(mfg[i] < 0x20 || mfg[i] > 0x7E) return false;
    }
    return true;
}

static NfcCommand sonicare_scene_quick_reset_poller_callback(NfcGenericEvent event, void* context) {
    furi_assert(context);
    Sonicare* app = context;

    if(event.protocol != NfcProtocolMfUltralight) {
        return NfcCommandContinue;
    }

    const MfUltralightPollerEvent* ev = event.event_data;
    NfcCommand command = NfcCommandContinue;

    if(ev->type == MfUltralightPollerEventTypeRequestMode) {
        ev->data->poller_mode = MfUltralightPollerModeRead;
    } else if(ev->type == MfUltralightPollerEventTypeAuthRequest) {
        // If tag requires authentication to read protected pages:
        const MfUltralightData* ul_data = (const MfUltralightData*)nfc_poller_get_data(app->poller);
        if(ul_data && ul_data->iso14443_3a_data && ul_data->pages_read >= 0x24) {
            uint8_t uid[7];
            uint8_t mfg[10];
            memcpy(uid, ul_data->iso14443_3a_data->uid, 7);
            memcpy(mfg + 0, ul_data->page[0x21].data + 2, 2);
            memcpy(mfg + 2, ul_data->page[0x22].data, 4);
            memcpy(mfg + 6, ul_data->page[0x23].data, 4);

            if(sonicare_quick_reset_validate_mfg(mfg)) {
                uint32_t pwd = get_sonicare_password(uid, mfg);
                FURI_LOG_I(TAG, "AuthRequest - providing password %08lX", pwd);
                ev->data->auth_context.password.data[0] = (pwd >> 24) & 0xFF;
                ev->data->auth_context.password.data[1] = (pwd >> 16) & 0xFF;
                ev->data->auth_context.password.data[2] = (pwd >> 8) & 0xFF;
                ev->data->auth_context.password.data[3] = (pwd >> 0) & 0xFF;
                ev->data->auth_context.skip_auth = false;
                return NfcCommandContinue;
            }
        }
        // If MFG not yet available, skip auth so read can complete first
        ev->data->auth_context.skip_auth = true;
    } else if(ev->type == MfUltralightPollerEventTypeAuthSuccess) {
        // Handled if card used AuthRequest path
        FURI_LOG_I(TAG, "AuthSuccess - resetting pages 0x24-0x26");
        const MfUltralightData* ul_data = (const MfUltralightData*)nfc_poller_get_data(app->poller);
        MfUltralightPoller* poller = (MfUltralightPoller*)event.instance;

        MfUltralightPage page24 = {.data = {0x00, 0x00, 0x02, 0x00}};
        if(ul_data) {
            page24.data[2] = ul_data->page[SONICARE_PAGE_USAGE].data[2];
            page24.data[3] = ul_data->page[SONICARE_PAGE_USAGE].data[3];
        }
        MfUltralightPage page_zero = {.data = {0x00, 0x00, 0x00, 0x00}};

        MfUltralightError err24 = mf_ultralight_poller_write_page(poller, SONICARE_PAGE_USAGE, &page24);
        MfUltralightError err25 = mf_ultralight_poller_write_page(poller, SONICARE_PAGE_USAGE + 1, &page_zero);
        MfUltralightError err26 = mf_ultralight_poller_write_page(poller, SONICARE_PAGE_USAGE + 2, &page_zero);

        if(err24 == MfUltralightErrorNone && err25 == MfUltralightErrorNone && err26 == MfUltralightErrorNone) {
            FURI_LOG_I(TAG, "Quick Reset Success!");
            if(ul_data && ul_data->iso14443_3a_data) {
                memcpy(app->quick_reset_last_uid, ul_data->iso14443_3a_data->uid, 7);
                app->quick_reset_has_last_uid = true;
            }
            view_dispatcher_send_custom_event(app->view_dispatcher, SonicareCustomEventQuickResetSuccess);
        } else {
            FURI_LOG_E(TAG, "Write failed");
            view_dispatcher_send_custom_event(app->view_dispatcher, SonicareCustomEventQuickResetFailed);
        }
        command = NfcCommandStop;
    } else if(ev->type == MfUltralightPollerEventTypeReadSuccess) {
        // Sonicare heads are unprotected for reading (PROT=0), so the poller finishes
        // read without needing auth. We authenticate now and write.
        FURI_LOG_I(TAG, "ReadSuccess - authenticating before writing");
        MfUltralightPoller* poller = (MfUltralightPoller*)event.instance;
        const MfUltralightData* ul_data = (const MfUltralightData*)nfc_poller_get_data(app->poller);

        if(!ul_data || !ul_data->iso14443_3a_data || ul_data->pages_read < 0x24) {
            FURI_LOG_W(TAG, "Incomplete read data");
            view_dispatcher_send_custom_event(app->view_dispatcher, SonicareCustomEventQuickResetFailed);
            return NfcCommandStop;
        }

        uint8_t uid[7];
        uint8_t mfg[10];
        memcpy(uid, ul_data->iso14443_3a_data->uid, 7);
        memcpy(mfg + 0, ul_data->page[0x21].data + 2, 2);
        memcpy(mfg + 2, ul_data->page[0x22].data, 4);
        memcpy(mfg + 6, ul_data->page[0x23].data, 4);

        // Prevent repeated resets while the same head is still held in place
        if(app->quick_reset_has_last_uid && memcmp(uid, app->quick_reset_last_uid, 7) == 0) {
            return NfcCommandContinue;
        }

        if(!sonicare_quick_reset_validate_mfg(mfg)) {
            FURI_LOG_W(TAG, "Invalid MFG string, aborting auth for safety");
            view_dispatcher_send_custom_event(app->view_dispatcher, SonicareCustomEventQuickResetFailed);
            return NfcCommandStop;
        }

        uint32_t pwd = get_sonicare_password(uid, mfg);
        FURI_LOG_I(TAG, "Authenticating with password 0x%08lX", pwd);

        MfUltralightPollerAuthContext auth_ctx = {};
        auth_ctx.password.data[0] = (pwd >> 24) & 0xFF;
        auth_ctx.password.data[1] = (pwd >> 16) & 0xFF;
        auth_ctx.password.data[2] = (pwd >> 8) & 0xFF;
        auth_ctx.password.data[3] = (pwd >> 0) & 0xFF;

        MfUltralightError auth_err = mf_ultralight_poller_auth_pwd(poller, &auth_ctx);
        if(auth_err != MfUltralightErrorNone) {
            FURI_LOG_E(TAG, "Auth failed: %d", auth_err);
            view_dispatcher_send_custom_event(app->view_dispatcher, SonicareCustomEventQuickResetAuthFailed);
            return NfcCommandStop;
        }

        // Reset page 0x24 = {00 00 02 00} (preserve intensity & mode)
        MfUltralightPage page24 = {.data = {0x00, 0x00, 0x02, 0x00}};
        page24.data[2] = ul_data->page[SONICARE_PAGE_USAGE].data[2];
        page24.data[3] = ul_data->page[SONICARE_PAGE_USAGE].data[3];
        MfUltralightPage page_zero = {.data = {0x00, 0x00, 0x00, 0x00}};

        MfUltralightError err24 = mf_ultralight_poller_write_page(poller, SONICARE_PAGE_USAGE, &page24);
        MfUltralightError err25 = mf_ultralight_poller_write_page(poller, SONICARE_PAGE_USAGE + 1, &page_zero);
        MfUltralightError err26 = mf_ultralight_poller_write_page(poller, SONICARE_PAGE_USAGE + 2, &page_zero);

        if(err24 == MfUltralightErrorNone && err25 == MfUltralightErrorNone && err26 == MfUltralightErrorNone) {
            FURI_LOG_I(TAG, "Quick Reset Success!");
            memcpy(app->quick_reset_last_uid, uid, 7);
            app->quick_reset_has_last_uid = true;
            view_dispatcher_send_custom_event(app->view_dispatcher, SonicareCustomEventQuickResetSuccess);
        } else {
            FURI_LOG_E(TAG, "Write failed: 24=%d, 25=%d, 26=%d", err24, err25, err26);
            view_dispatcher_send_custom_event(app->view_dispatcher, SonicareCustomEventQuickResetFailed);
        }
        command = NfcCommandStop;
    } else if(ev->type == MfUltralightPollerEventTypeAuthFailed) {
        FURI_LOG_E(TAG, "Auth failed");
        view_dispatcher_send_custom_event(app->view_dispatcher, SonicareCustomEventQuickResetAuthFailed);
        command = NfcCommandStop;
    }

    return command;
}

void sonicare_scene_quick_reset_on_enter(void* context) {
    Sonicare* app = context;
    Popup* popup = app->popup;

    app->quick_reset_has_last_uid = false;
    memset(app->quick_reset_last_uid, 0, sizeof(app->quick_reset_last_uid));

    popup_reset(popup);
    popup_set_header(popup, "Quick Reset", 83, 8, AlignCenter, AlignTop);
    popup_set_text(popup, "Tap brush stem\nagainst\nFlipper's back", 83, 27, AlignCenter, AlignTop);
    popup_set_icon(popup, 0, 0, &I_sonicare_read);
    view_dispatcher_switch_to_view(app->view_dispatcher, SonicareViewPopup);

    app->poller = nfc_poller_alloc(app->nfc, NfcProtocolMfUltralight);
    nfc_poller_start(app->poller, sonicare_scene_quick_reset_poller_callback, app);
}

bool sonicare_scene_quick_reset_on_event(void* context, SceneManagerEvent event) {
    Sonicare* app = context;
    Popup* popup = app->popup;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == SonicareCustomEventQuickResetSuccess) {
            notification_message(app->notifications, &sequence_success);
            dolphin_deed(DolphinDeedNfcRead);

            popup_reset(popup);
            popup_set_header(popup, "Reset OK!", 83, 8, AlignCenter, AlignTop);
            popup_set_text(popup, "Usage: 0 min\n\nTap next head...", 83, 27, AlignCenter, AlignTop);
            popup_set_icon(popup, 0, 0, &I_sonicare_brush);

            // Re-arm poller for next head
            if(app->poller) {
                nfc_poller_stop(app->poller);
                nfc_poller_free(app->poller);
            }
            app->poller = nfc_poller_alloc(app->nfc, NfcProtocolMfUltralight);
            nfc_poller_start(app->poller, sonicare_scene_quick_reset_poller_callback, app);

            consumed = true;
        } else if(event.event == SonicareCustomEventQuickResetFailed) {
            notification_message(app->notifications, &sequence_error);
            popup_reset(popup);
            popup_set_header(popup, "Write Error", 83, 8, AlignCenter, AlignTop);
            popup_set_text(popup, "Failed to write\n\nPress Back to exit", 83, 27, AlignCenter, AlignTop);

            // Stop poller on failure to prevent infinite vibration loop
            if(app->poller) {
                nfc_poller_stop(app->poller);
                nfc_poller_free(app->poller);
                app->poller = NULL;
            }

            consumed = true;
        } else if(event.event == SonicareCustomEventQuickResetAuthFailed) {
            notification_message(app->notifications, &sequence_error);
            popup_reset(popup);
            popup_set_header(popup, "Auth Failed", 83, 8, AlignCenter, AlignTop);
            popup_set_text(popup, "AUTHLIM protection\n\nPress Back to exit", 83, 27, AlignCenter, AlignTop);

            // Stop poller on auth fail to protect card from repeated attempts
            if(app->poller) {
                nfc_poller_stop(app->poller);
                nfc_poller_free(app->poller);
                app->poller = NULL;
            }

            consumed = true;
        }
    }

    return consumed;
}

void sonicare_scene_quick_reset_on_exit(void* context) {
    Sonicare* app = context;

    if(app->poller) {
        nfc_poller_stop(app->poller);
        nfc_poller_free(app->poller);
        app->poller = NULL;
    }
    app->quick_reset_has_last_uid = false;
    memset(app->quick_reset_last_uid, 0, sizeof(app->quick_reset_last_uid));
    popup_reset(app->popup);
}
