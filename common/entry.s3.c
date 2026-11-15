#include <stddef.h>
#include <stdint.h>
#include <stdnoreturn.h>
#include <lib/term.h>
#include <lib/real.h>
#include <lib/misc.h>
#include <lib/libc.h>
#include <lib/part.h>
#include <lib/config.h>
#include <lib/trace.h>
#include <lib/bli.h>
#include <lib/tpm.h>
#include <sys/e820.h>
#include <sys/a20.h>
#include <sys/idt.h>
#include <sys/gdt.h>
#include <lib/print.h>
#include <fs/file.h>
#include <lib/elf.h>
#include <mm/pmm.h>
#include <menu.h>
#include <pxe/pxe.h>
#include <pxe/tftp.h>
#include <drivers/disk.h>
#include <sys/lapic.h>
#include <lib/getchar.h>
#include <sys/cpu.h>
#include <libfdt.h>

void stage3_common(void);

#if defined(UEFI)
noreturn void uefi_entry(efi_handle image_handle, struct efi_system_table *system_table) {
    gST = system_table;
    gBS = system_table->boot_services;
    gRT = system_table->runtime_services;
    efi_image_handle = image_handle;

    reseed_stack_guard();

    calibrate_tsc();
    usec_at_bootloader_entry = rdtsc_usec();

    efi_status status;

    gST->con_out->enable_cursor(gST->con_out, false);

    init_memmap();

    term_fallback();

    status = gBS->set_watchdog_timer(0, 0x10000, 0, NULL);
    if (status) {
        print("WARNING: Failed to disable watchdog timer!\n");
    }

    init_gdt();
#if defined(__x86_64__)
    // Stage the below-4GiB handoff stub while allocations are still permitted.
    prepare_spinup_tramp();
#endif

    disk_create_index();

    // Detect UEFI Secure Boot
    {
        struct efi_guid global_variable = EFI_GLOBAL_VARIABLE;
        uint8_t secure_boot = 0;
        size_t sb_size = sizeof(secure_boot);
        efi_status sb_status = gRT->get_variable(L"SecureBoot", &global_variable, NULL, &sb_size, &secure_boot);
        if (sb_status == EFI_SUCCESS && secure_boot == 1) {
            uint8_t setup_mode = 0;
            size_t sm_size = sizeof(setup_mode);
            efi_status sm_status = gRT->get_variable(L"SetupMode", &global_variable, NULL, &sm_size, &setup_mode);
            if (sm_status != EFI_SUCCESS || setup_mode == 0) {
                secure_boot_active = true;
            }
        }
    }

    tpm_init();

    boot_volume = NULL;

    efi_handle current_handle = image_handle;
    for (size_t j = 0; j < 25; j++) {
        if (current_handle == NULL) {
could_not_match:
            print("WARNING: Could not meaningfully match the boot device handle with a volume.\n");
            print("         Using the first volume containing a Initium configuration!\n");
            print("\n");
            print("THIS IS A BUG! Please report this issue upstream.\n");
            print("Press any key to continue...\n");
            for (;;) {
                int ret = pit_sleep_and_quit_on_keypress(65535);
                if (ret != 0) {
                    break;
                }
            }

            for (size_t i = 0; i < volume_index_i; i++) {
                struct file_handle *f;

                bool old_cif = case_insensitive_fopen;
                case_insensitive_fopen = true;
                if (
                 false
#if defined(UEFI)
                 || (f = fopen(volume_index[i], "/EFI/initium/initium.conf")) != NULL
                 || (f = fopen(volume_index[i], "/EFI/BOOT/initium.conf")) != NULL
#endif
                 || (f = fopen(volume_index[i], "/boot/initium/initium.conf")) != NULL
                 || (f = fopen(volume_index[i], "/boot/initium.conf")) != NULL
                 || (f = fopen(volume_index[i], "/initium/initium.conf")) != NULL
                 || (f = fopen(volume_index[i], "/initium.conf")) != NULL
                ) {
                    goto opened;
                }

                case_insensitive_fopen = old_cif;
                continue;

opened:
                case_insensitive_fopen = old_cif;

                fclose(f);

                if (volume_index[i]->backing_dev != NULL) {
                    boot_volume = volume_index[i]->backing_dev;
                } else {
                    boot_volume = volume_index[i];
                }

                break;
            }

            if (boot_volume != NULL) {
                stage3_common();
            }

            panic(false, "No volume contained a Initium configuration file");
        }

        struct efi_guid loaded_img_prot_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
        struct efi_loaded_image_protocol *loaded_image = NULL;

        status = gBS->handle_protocol(current_handle, &loaded_img_prot_guid,
                                     (void **)&loaded_image);

        if (status) {
            goto could_not_match;
        }

        boot_volume = disk_volume_from_efi_handle(loaded_image->device_handle);

        if (boot_volume != NULL) {
            stage3_common();
        }

        current_handle = loaded_image->parent_handle;
    }

    goto could_not_match;
}
#endif


noreturn void stage3_common(void) {
    init_flush_irqs();
    init_io_apics();


    term_notready();

#if defined(UEFI)
    init_bli();
#endif

    menu(true);
}
