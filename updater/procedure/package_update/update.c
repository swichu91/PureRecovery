#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <common/enum_s.h>
#include <hal/security.h>
#include <hal/hwcrypt/signature.h>
#include "common/trace.h"
#include "update.h"
#include "priv_update.h"
#include "priv_tmp.h"
#include "procedure/checksum/checksum.h"
#include "procedure/version/version.h"
#include "procedure/backup/backup.h"
#include "procedure/package_update/update_ecoboot.h"

const char *update_strerror(int e)
{
    switch (e)
    {
        ENUMS(ErrorUpdateOk);
        ENUMS(ErrorSignCheck);
        ENUMS(ErrorUnpack);
        ENUMS(ErrorBackup);
        ENUMS(ErrorTmp);
        ENUMS(ErrorChecksums);
        ENUMS(ErrorVersion);
        ENUMS(ErrorMove);
        ENUMS(ErrorUpdateEcoboot);
    }
    return "UNKNOWN";
}

static const char *update_strerr_ext(int err, int ext_err)
{
    if (err == ErrorSignCheck)
    {
        switch (ext_err)
        {
            ENUMS(sec_verify_ok);
            ENUMS(sec_verify_openconfig);
            ENUMS(sec_verify_invalsign);
            ENUMS(sec_verify_ioerror);
            ENUMS(sec_verify_invalevt);
            ENUMS(sec_verify_confopen);
            ENUMS(sec_verify_invalid_sha);
        }
    }
    return "";
}

static void str_clean_up(char **str)
{
    if (str)
    {
        free(*str);
    }
}

static int signature_check(const char *name)
{
    if (sec_configuration_is_open())
    {
        return sec_verify_ok;
    }
    const char sig_ext[] = ".sig";
    char *signature_name __attribute__((__cleanup__(str_clean_up))) = malloc(strlen(name) + sizeof(sig_ext));
    strcpy(signature_name, name);
    strcat(signature_name, sig_ext);
    return sec_verify_file(name, signature_name);
}

void update_firmware_init(struct update_handle_s *h)
{
    memset(h, 0, sizeof *h);
}

bool update_firmware(struct update_handle_s *handle, trace_list_t *tl)
{
    trace_t *t = trace_append("update", tl, update_strerror, update_strerr_ext);

    struct backup_handle_s backup_handle = {.backup_from_os = handle->update_os,
                                            .backup_from_user = handle->update_user,
                                            .backup_to = handle->backup_full_path};
    if (handle->enabled.check_sign)
    {
        const int err = signature_check(handle->update_from);
        if (err)
        {
            handle->unsigned_tar = true;
        }
        else
        {
            handle->unsigned_tar = false;
        }
        printf("Package is signed: %s\n", handle->unsigned_tar ? "FALSE" : "TRUE");
    }
    else
    {
        printf("Package signature check skipped\n");
    }

    if (handle->enabled.backup)
    {
        if (handle->enabled.backup && !backup_previous_firmware(&backup_handle, tl))
        {
            trace_write(t, ErrorBackup, 0);
            goto exit;
        }
    }

    if (!tmp_create_catalog(handle, tl))
    {
        trace_write(t, ErrorBackup, 0);
        goto exit;
    }

    if (!unpack(handle, tl))
    {
        trace_write(t, ErrorUnpack, 0);
        goto exit;
    }

    if (handle->enabled.check_checksum || handle->enabled.check_version) {
        verify_file_handle_s verify_handle = json_get_verify_files(t, "/os/tmp/version.json", "/os/current/version.json");

        if (handle->enabled.check_checksum) {
            if (!checksum_verify_all(tl, &verify_handle, handle->tmp_os)) {
                trace_write(t, ErrorChecksums, 0);
                goto exit;
            }
        }
        if (handle->enabled.check_version) {
            if (!version_check_all(tl, &verify_handle, handle->tmp_os)) {
                trace_write(t, ErrorVersion, 0);
                goto exit;
            }
        }
    }

    if (!tmp_files_move(handle, tl))
    {
        trace_write(t, ErrorMove, 0);
        goto exit;
    }

    // Finally update the ecoboot bin
    int ecoboot_package_status = ecoboot_in_package(handle->update_os, "ecoboot.bin");
    if (ecoboot_package_status == 1)
    {
        const int eco_status = ecoboot_update(handle->update_os, "ecoboot.bin", tl);
        if (eco_status != error_eco_update_ok)
        {
            if (eco_status != error_eco_vfs && errno != ENOENT)
            {
                trace_write(t, ErrorUpdateEcoboot, 0);
                goto exit;
            }
        }
        else
        {
            printf("Ecoboot update success\n");
        }
    }
    else
    {
        printf("No ecoboot.bin in package, %d\n", ecoboot_package_status);
    }

exit:
    return trace_ok(t);
}
