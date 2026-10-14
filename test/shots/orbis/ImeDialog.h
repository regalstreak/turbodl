// host shim for the PS4 IME dialog header (screenshot renderer only)
#pragma once
#include <stdint.h>
#include <wchar.h>
typedef enum { ORBIS_DIALOG_OK = 0, ORBIS_DIALOG_CANCEL = 1, ORBIS_DIALOG_ABORD = 2 } OrbisDialogResultStatus;
typedef enum { ORBIS_TYPE_DEFAULT = 0, ORBIS_TYPE_BASIC_LATIN, ORBIS_TYPE_TYPE_URL, ORBIS_TYPE_MAIL, ORBIS_TYPE_NUMBER } OrbisImeType;
typedef enum { ORBIS_BUTTON_LABEL_DEFAULT = 0, ORBIS_BUTTON_LABEL_SEND, ORBIS_BUTTON_LABEL_SEARCH, ORBIS_BUTTON_LABEL_GO } OrbisButtonLabel;
typedef enum { ORBIS__DEFAULT = 0 } OrbisInput;
typedef enum { ORBIS_DIALOG_STATUS_NONE = 0, ORBIS_DIALOG_STATUS_RUNNING, ORBIS_DIALOG_STATUS_STOPPED } OrbisDialogStatus;
typedef enum { ORBIS_V_TOP = 0, ORBIS_V_CENTER = 1, ORBIS_V_VALIGN_BOTTOM = 2 } OrbisVAlignment;
typedef enum { ORBIS_H_LEFT = 0, ORBIS_H_CENTER = 1, ORBIS_H_RIGHT = 2 } OrbisHAlignment;
typedef struct { OrbisDialogResultStatus endstatus; int8_t reserved[12]; } OrbisDialogResult;
typedef struct {
    uint32_t userId; OrbisImeType type; uint64_t supportedLanguages; OrbisButtonLabel enterLabel;
    OrbisInput inputMethod; void *filter; uint32_t option; uint32_t maxTextLength; wchar_t *inputTextBuffer;
    float posx, posy; OrbisHAlignment horizontalAlignment; OrbisVAlignment verticalAlignment;
    const wchar_t *placeholder; const wchar_t *title; int8_t reserved[16];
} OrbisImeDialogSetting;
typedef struct { uint32_t option; int8_t reserved[100]; } OrbisImeSettingsExtended;
int32_t sceImeDialogInit(const OrbisImeDialogSetting *p, OrbisImeSettingsExtended *e);
OrbisDialogStatus sceImeDialogGetStatus(void);
int32_t sceImeDialogGetResult(OrbisDialogResult *r);
int32_t sceImeDialogTerm(void);
