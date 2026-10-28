################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Core/Src/audio_resample.c \
../Core/Src/board_rev1.c \
../Core/Src/cat.c \
../Core/Src/cat_logic.c \
../Core/Src/clock_plan.c \
../Core/Src/cw_pack.c \
../Core/Src/fw_update.c \
../Core/Src/hrtim_phase.c \
../Core/Src/main.c \
../Core/Src/mod_api.c \
../Core/Src/mod_api_audio.c \
../Core/Src/mod_api_cw.c \
../Core/Src/mod_api_mode.c \
../Core/Src/mod_api_pa.c \
../Core/Src/mod_api_ui_diag.c \
../Core/Src/mod_band.c \
../Core/Src/mod_cal_storage.c \
../Core/Src/mod_cpu_diag.c \
../Core/Src/mod_crc32.c \
../Core/Src/mod_cw.c \
../Core/Src/mod_cw_keyer.c \
../Core/Src/mod_cw_player.c \
../Core/Src/mod_dsp_tables.c \
../Core/Src/mod_morse_tables.c \
../Core/Src/mod_morse_trainer.c \
../Core/Src/mod_outphase.c \
../Core/Src/mod_sd_store.c \
../Core/Src/mod_sdcard.c \
../Core/Src/mod_sdcard_service.c \
../Core/Src/mod_selftest.c \
../Core/Src/mod_selftest_logic.c \
../Core/Src/mod_stack_watermark.c \
../Core/Src/mod_state.c \
../Core/Src/mod_storage.c \
../Core/Src/mod_storage_format.c \
../Core/Src/mod_storage_import.c \
../Core/Src/mod_transport.c \
../Core/Src/mod_transport_logic.c \
../Core/Src/mod_tx_pipeline.c \
../Core/Src/modulator.c \
../Core/Src/radio.c \
../Core/Src/radio_af_fit.c \
../Core/Src/radio_af_sweep.c \
../Core/Src/radio_analog.c \
../Core/Src/radio_audio.c \
../Core/Src/radio_bpf_fit.c \
../Core/Src/radio_bpf_sweep.c \
../Core/Src/radio_clocks.c \
../Core/Src/radio_config.c \
../Core/Src/radio_cw_decoder.c \
../Core/Src/radio_digi.c \
../Core/Src/radio_dsp.c \
../Core/Src/radio_iq_fit.c \
../Core/Src/radio_pd.c \
../Core/Src/radio_pd_policy.c \
../Core/Src/radio_pd_port.c \
../Core/Src/radio_rf_meter.c \
../Core/Src/radio_rx_cal.c \
../Core/Src/radio_rx_front.c \
../Core/Src/radio_slot_logic.c \
../Core/Src/radio_spectrum.c \
../Core/Src/sctrl_link.c \
../Core/Src/sctrl_link_logic.c \
../Core/Src/sd_audio_decoder.c \
../Core/Src/sd_audio_math.c \
../Core/Src/sd_audio_player.c \
../Core/Src/sd_diskio.c \
../Core/Src/sd_file_browser.c \
../Core/Src/stm32g4xx_hal_msp.c \
../Core/Src/stm32g4xx_it.c \
../Core/Src/syscalls.c \
../Core/Src/sysmem.c \
../Core/Src/system_stm32g4xx.c \
../Core/Src/ui_app.c \
../Core/Src/ui_diag.c \
../Core/Src/ui_format.c \
../Core/Src/ui_gfx.c \
../Core/Src/ui_help.c \
../Core/Src/ui_input.c \
../Core/Src/ui_labels.c \
../Core/Src/ui_lcd.c \
../Core/Src/ui_menu.c \
../Core/Src/ui_nav.c \
../Core/Src/ui_render.c \
../Core/Src/ui_render_menu.c \
../Core/Src/ui_render_pages.c \
../Core/Src/ui_sd.c \
../Core/Src/ui_settings_map.c \
../Core/Src/ui_trainer.c \
../Core/Src/ui_tune.c \
../Core/Src/ui_waterfall.c \
../Core/Src/usb_diag.c \
../Core/Src/usb_diag_keys.c \
../Core/Src/usb_diag_mode.c \
../Core/Src/usb_diag_pins.c \
../Core/Src/usb_disk_mode.c \
../Core/Src/usbd_msc_if.c

OBJS += \
./Core/Src/audio_resample.o \
./Core/Src/board_rev1.o \
./Core/Src/cat.o \
./Core/Src/cat_logic.o \
./Core/Src/clock_plan.o \
./Core/Src/cw_pack.o \
./Core/Src/fw_update.o \
./Core/Src/hrtim_phase.o \
./Core/Src/main.o \
./Core/Src/mod_api.o \
./Core/Src/mod_api_audio.o \
./Core/Src/mod_api_cw.o \
./Core/Src/mod_api_mode.o \
./Core/Src/mod_api_pa.o \
./Core/Src/mod_api_ui_diag.o \
./Core/Src/mod_band.o \
./Core/Src/mod_cal_storage.o \
./Core/Src/mod_cpu_diag.o \
./Core/Src/mod_crc32.o \
./Core/Src/mod_cw.o \
./Core/Src/mod_cw_keyer.o \
./Core/Src/mod_cw_player.o \
./Core/Src/mod_dsp_tables.o \
./Core/Src/mod_morse_tables.o \
./Core/Src/mod_morse_trainer.o \
./Core/Src/mod_outphase.o \
./Core/Src/mod_sd_store.o \
./Core/Src/mod_sdcard.o \
./Core/Src/mod_sdcard_service.o \
./Core/Src/mod_selftest.o \
./Core/Src/mod_selftest_logic.o \
./Core/Src/mod_stack_watermark.o \
./Core/Src/mod_state.o \
./Core/Src/mod_storage.o \
./Core/Src/mod_storage_format.o \
./Core/Src/mod_storage_import.o \
./Core/Src/mod_transport.o \
./Core/Src/mod_transport_logic.o \
./Core/Src/mod_tx_pipeline.o \
./Core/Src/modulator.o \
./Core/Src/radio.o \
./Core/Src/radio_af_fit.o \
./Core/Src/radio_af_sweep.o \
./Core/Src/radio_analog.o \
./Core/Src/radio_audio.o \
./Core/Src/radio_bpf_fit.o \
./Core/Src/radio_bpf_sweep.o \
./Core/Src/radio_clocks.o \
./Core/Src/radio_config.o \
./Core/Src/radio_cw_decoder.o \
./Core/Src/radio_digi.o \
./Core/Src/radio_dsp.o \
./Core/Src/radio_iq_fit.o \
./Core/Src/radio_pd.o \
./Core/Src/radio_pd_policy.o \
./Core/Src/radio_pd_port.o \
./Core/Src/radio_rf_meter.o \
./Core/Src/radio_rx_cal.o \
./Core/Src/radio_rx_front.o \
./Core/Src/radio_slot_logic.o \
./Core/Src/radio_spectrum.o \
./Core/Src/sctrl_link.o \
./Core/Src/sctrl_link_logic.o \
./Core/Src/sd_audio_decoder.o \
./Core/Src/sd_audio_math.o \
./Core/Src/sd_audio_player.o \
./Core/Src/sd_diskio.o \
./Core/Src/sd_file_browser.o \
./Core/Src/stm32g4xx_hal_msp.o \
./Core/Src/stm32g4xx_it.o \
./Core/Src/syscalls.o \
./Core/Src/sysmem.o \
./Core/Src/system_stm32g4xx.o \
./Core/Src/ui_app.o \
./Core/Src/ui_diag.o \
./Core/Src/ui_format.o \
./Core/Src/ui_gfx.o \
./Core/Src/ui_help.o \
./Core/Src/ui_input.o \
./Core/Src/ui_labels.o \
./Core/Src/ui_lcd.o \
./Core/Src/ui_menu.o \
./Core/Src/ui_nav.o \
./Core/Src/ui_render.o \
./Core/Src/ui_render_menu.o \
./Core/Src/ui_render_pages.o \
./Core/Src/ui_sd.o \
./Core/Src/ui_settings_map.o \
./Core/Src/ui_trainer.o \
./Core/Src/ui_tune.o \
./Core/Src/ui_waterfall.o \
./Core/Src/usb_diag.o \
./Core/Src/usb_diag_keys.o \
./Core/Src/usb_diag_mode.o \
./Core/Src/usb_diag_pins.o \
./Core/Src/usb_disk_mode.o \
./Core/Src/usbd_msc_if.o

C_DEPS += \
./Core/Src/audio_resample.d \
./Core/Src/board_rev1.d \
./Core/Src/cat.d \
./Core/Src/cat_logic.d \
./Core/Src/clock_plan.d \
./Core/Src/cw_pack.d \
./Core/Src/fw_update.d \
./Core/Src/hrtim_phase.d \
./Core/Src/main.d \
./Core/Src/mod_api.d \
./Core/Src/mod_api_audio.d \
./Core/Src/mod_api_cw.d \
./Core/Src/mod_api_mode.d \
./Core/Src/mod_api_pa.d \
./Core/Src/mod_api_ui_diag.d \
./Core/Src/mod_band.d \
./Core/Src/mod_cal_storage.d \
./Core/Src/mod_cpu_diag.d \
./Core/Src/mod_crc32.d \
./Core/Src/mod_cw.d \
./Core/Src/mod_cw_keyer.d \
./Core/Src/mod_cw_player.d \
./Core/Src/mod_dsp_tables.d \
./Core/Src/mod_morse_tables.d \
./Core/Src/mod_morse_trainer.d \
./Core/Src/mod_outphase.d \
./Core/Src/mod_sd_store.d \
./Core/Src/mod_sdcard.d \
./Core/Src/mod_sdcard_service.d \
./Core/Src/mod_selftest.d \
./Core/Src/mod_selftest_logic.d \
./Core/Src/mod_stack_watermark.d \
./Core/Src/mod_state.d \
./Core/Src/mod_storage.d \
./Core/Src/mod_storage_format.d \
./Core/Src/mod_storage_import.d \
./Core/Src/mod_transport.d \
./Core/Src/mod_transport_logic.d \
./Core/Src/mod_tx_pipeline.d \
./Core/Src/modulator.d \
./Core/Src/radio.d \
./Core/Src/radio_af_fit.d \
./Core/Src/radio_af_sweep.d \
./Core/Src/radio_analog.d \
./Core/Src/radio_audio.d \
./Core/Src/radio_bpf_fit.d \
./Core/Src/radio_bpf_sweep.d \
./Core/Src/radio_clocks.d \
./Core/Src/radio_config.d \
./Core/Src/radio_cw_decoder.d \
./Core/Src/radio_digi.d \
./Core/Src/radio_dsp.d \
./Core/Src/radio_iq_fit.d \
./Core/Src/radio_pd.d \
./Core/Src/radio_pd_policy.d \
./Core/Src/radio_pd_port.d \
./Core/Src/radio_rf_meter.d \
./Core/Src/radio_rx_cal.d \
./Core/Src/radio_rx_front.d \
./Core/Src/radio_slot_logic.d \
./Core/Src/radio_spectrum.d \
./Core/Src/sctrl_link.d \
./Core/Src/sctrl_link_logic.d \
./Core/Src/sd_audio_decoder.d \
./Core/Src/sd_audio_math.d \
./Core/Src/sd_audio_player.d \
./Core/Src/sd_diskio.d \
./Core/Src/sd_file_browser.d \
./Core/Src/stm32g4xx_hal_msp.d \
./Core/Src/stm32g4xx_it.d \
./Core/Src/syscalls.d \
./Core/Src/sysmem.d \
./Core/Src/system_stm32g4xx.d \
./Core/Src/ui_app.d \
./Core/Src/ui_diag.d \
./Core/Src/ui_format.d \
./Core/Src/ui_gfx.d \
./Core/Src/ui_help.d \
./Core/Src/ui_input.d \
./Core/Src/ui_labels.d \
./Core/Src/ui_lcd.d \
./Core/Src/ui_menu.d \
./Core/Src/ui_nav.d \
./Core/Src/ui_render.d \
./Core/Src/ui_render_menu.d \
./Core/Src/ui_render_pages.d \
./Core/Src/ui_sd.d \
./Core/Src/ui_settings_map.d \
./Core/Src/ui_trainer.d \
./Core/Src/ui_tune.d \
./Core/Src/ui_waterfall.d \
./Core/Src/usb_diag.d \
./Core/Src/usb_diag_keys.d \
./Core/Src/usb_diag_mode.d \
./Core/Src/usb_diag_pins.d \
./Core/Src/usb_disk_mode.d \
./Core/Src/usbd_msc_if.d


# Section anchors (hand-added; keep it when CubeIDE regenerates this file).
# Without -fdata-sections GCC addresses a file's statics from one anchor
# instead of one literal-pool word and load per variable: smaller and fewer
# flash reads. Only files where that saves much flash for little RAM (their
# .bss is laid out as one block, with its padding) are listed.
CORE_SRC_ANCHORED := radio_audio radio_pd radio board_rev1 mod_tx_pipeline mod_sdcard sd_file_browser ui_sd mod_storage sd_audio_player radio_analog mod_morse_trainer sd_audio_decoder mod_storage_import hrtim_phase radio_clocks stm32g4xx_it usb_diag_mode mod_sd_store
$(CORE_SRC_ANCHORED:%=Core/Src/%.o): Core/Src/%.o: ../Core/Src/%.c Core/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DUSE_FULL_LL_DRIVER -DUSBPD_PORT_COUNT=1 -D_SNK -DUSBPDCORE_LIB_PD3_CONFIG_1 -DDEBUG -DUSE_HAL_DRIVER -DSTM32G474xx -c -I../Middlewares/ST/STM32_USBPD_Library/Core/inc -I../Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/inc -I../Core/Inc -I../Drivers/STM32G4xx_HAL_Driver/Inc -I../Drivers/STM32G4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32G4xx/Include -I../Drivers/CMSIS/Include -I../USB_Device/App -I../USB_Device/Target -I../Middlewares/ST/STM32_USB_Device_Library/Core/Inc -I../Middlewares/ST/STM32_USB_Device_Library/Class/AUDIO/Inc -I../Middlewares/ST/STM32_USB_Device_Library/Class/MSC/Inc -O2 -ffunction-sections -Wall -fno-math-errno -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

# Each subdirectory must supply rules for building sources it contributes
Core/Src/%.o Core/Src/%.su Core/Src/%.cyclo: ../Core/Src/%.c Core/Src/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DUSE_FULL_LL_DRIVER -DUSBPD_PORT_COUNT=1 -D_SNK -DUSBPDCORE_LIB_PD3_CONFIG_1 -DDEBUG -DUSE_HAL_DRIVER -DSTM32G474xx -c -I../Middlewares/ST/STM32_USBPD_Library/Core/inc -I../Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/inc -I../Core/Inc -I../Drivers/STM32G4xx_HAL_Driver/Inc -I../Drivers/STM32G4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32G4xx/Include -I../Drivers/CMSIS/Include -I../USB_Device/App -I../USB_Device/Target -I../Middlewares/ST/STM32_USB_Device_Library/Core/Inc -I../Middlewares/ST/STM32_USB_Device_Library/Class/AUDIO/Inc -I../Middlewares/ST/STM32_USB_Device_Library/Class/MSC/Inc -O2 -ffunction-sections -fdata-sections -Wall -fno-math-errno -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Core-2f-Src

clean-Core-2f-Src:
	-$(RM) ./Core/Src/audio_resample.cyclo ./Core/Src/audio_resample.d ./Core/Src/audio_resample.o ./Core/Src/audio_resample.su ./Core/Src/board_rev1.cyclo ./Core/Src/board_rev1.d ./Core/Src/board_rev1.o ./Core/Src/board_rev1.su ./Core/Src/cat.cyclo ./Core/Src/cat.d ./Core/Src/cat.o ./Core/Src/cat.su ./Core/Src/cat_logic.cyclo ./Core/Src/cat_logic.d ./Core/Src/cat_logic.o ./Core/Src/cat_logic.su ./Core/Src/clock_plan.cyclo ./Core/Src/clock_plan.d ./Core/Src/clock_plan.o ./Core/Src/clock_plan.su ./Core/Src/cw_pack.cyclo ./Core/Src/cw_pack.d ./Core/Src/cw_pack.o ./Core/Src/cw_pack.su ./Core/Src/fw_update.cyclo ./Core/Src/fw_update.d ./Core/Src/fw_update.o ./Core/Src/fw_update.su ./Core/Src/hrtim_phase.cyclo ./Core/Src/hrtim_phase.d ./Core/Src/hrtim_phase.o ./Core/Src/hrtim_phase.su ./Core/Src/main.cyclo ./Core/Src/main.d ./Core/Src/main.o ./Core/Src/main.su ./Core/Src/mod_api.cyclo ./Core/Src/mod_api.d ./Core/Src/mod_api.o ./Core/Src/mod_api.su ./Core/Src/mod_api_audio.cyclo ./Core/Src/mod_api_audio.d ./Core/Src/mod_api_audio.o ./Core/Src/mod_api_audio.su ./Core/Src/mod_api_cw.cyclo ./Core/Src/mod_api_cw.d ./Core/Src/mod_api_cw.o ./Core/Src/mod_api_cw.su ./Core/Src/mod_api_mode.cyclo ./Core/Src/mod_api_mode.d ./Core/Src/mod_api_mode.o ./Core/Src/mod_api_mode.su ./Core/Src/mod_api_pa.cyclo ./Core/Src/mod_api_pa.d ./Core/Src/mod_api_pa.o ./Core/Src/mod_api_pa.su ./Core/Src/mod_api_ui_diag.cyclo ./Core/Src/mod_api_ui_diag.d ./Core/Src/mod_api_ui_diag.o ./Core/Src/mod_api_ui_diag.su ./Core/Src/mod_band.cyclo ./Core/Src/mod_band.d ./Core/Src/mod_band.o ./Core/Src/mod_band.su ./Core/Src/mod_cal_storage.cyclo ./Core/Src/mod_cal_storage.d ./Core/Src/mod_cal_storage.o ./Core/Src/mod_cal_storage.su ./Core/Src/mod_cpu_diag.cyclo ./Core/Src/mod_cpu_diag.d ./Core/Src/mod_cpu_diag.o ./Core/Src/mod_cpu_diag.su ./Core/Src/mod_crc32.cyclo ./Core/Src/mod_crc32.d ./Core/Src/mod_crc32.o ./Core/Src/mod_crc32.su ./Core/Src/mod_cw.cyclo ./Core/Src/mod_cw.d ./Core/Src/mod_cw.o ./Core/Src/mod_cw.su ./Core/Src/mod_cw_keyer.cyclo ./Core/Src/mod_cw_keyer.d ./Core/Src/mod_cw_keyer.o ./Core/Src/mod_cw_keyer.su ./Core/Src/mod_cw_player.cyclo ./Core/Src/mod_cw_player.d ./Core/Src/mod_cw_player.o ./Core/Src/mod_cw_player.su ./Core/Src/mod_dsp_tables.cyclo ./Core/Src/mod_dsp_tables.d ./Core/Src/mod_dsp_tables.o ./Core/Src/mod_dsp_tables.su ./Core/Src/mod_morse_tables.cyclo ./Core/Src/mod_morse_tables.d ./Core/Src/mod_morse_tables.o ./Core/Src/mod_morse_tables.su ./Core/Src/mod_morse_trainer.cyclo ./Core/Src/mod_morse_trainer.d ./Core/Src/mod_morse_trainer.o ./Core/Src/mod_morse_trainer.su ./Core/Src/mod_outphase.cyclo ./Core/Src/mod_outphase.d ./Core/Src/mod_outphase.o ./Core/Src/mod_outphase.su ./Core/Src/mod_sd_store.cyclo ./Core/Src/mod_sd_store.d ./Core/Src/mod_sd_store.o ./Core/Src/mod_sd_store.su ./Core/Src/mod_sdcard.cyclo ./Core/Src/mod_sdcard.d ./Core/Src/mod_sdcard.o ./Core/Src/mod_sdcard.su ./Core/Src/mod_sdcard_service.cyclo ./Core/Src/mod_sdcard_service.d ./Core/Src/mod_sdcard_service.o ./Core/Src/mod_sdcard_service.su ./Core/Src/mod_selftest.cyclo ./Core/Src/mod_selftest.d ./Core/Src/mod_selftest.o ./Core/Src/mod_selftest.su ./Core/Src/mod_selftest_logic.cyclo ./Core/Src/mod_selftest_logic.d ./Core/Src/mod_selftest_logic.o ./Core/Src/mod_selftest_logic.su ./Core/Src/mod_stack_watermark.cyclo ./Core/Src/mod_stack_watermark.d ./Core/Src/mod_stack_watermark.o ./Core/Src/mod_stack_watermark.su ./Core/Src/mod_state.cyclo ./Core/Src/mod_state.d ./Core/Src/mod_state.o ./Core/Src/mod_state.su ./Core/Src/mod_storage.cyclo ./Core/Src/mod_storage.d ./Core/Src/mod_storage.o ./Core/Src/mod_storage.su ./Core/Src/mod_storage_format.cyclo ./Core/Src/mod_storage_format.d ./Core/Src/mod_storage_format.o ./Core/Src/mod_storage_format.su ./Core/Src/mod_storage_import.cyclo ./Core/Src/mod_storage_import.d ./Core/Src/mod_storage_import.o ./Core/Src/mod_storage_import.su ./Core/Src/mod_transport.cyclo ./Core/Src/mod_transport.d ./Core/Src/mod_transport.o ./Core/Src/mod_transport.su ./Core/Src/mod_transport_logic.cyclo ./Core/Src/mod_transport_logic.d ./Core/Src/mod_transport_logic.o ./Core/Src/mod_transport_logic.su ./Core/Src/mod_tx_pipeline.cyclo ./Core/Src/mod_tx_pipeline.d ./Core/Src/mod_tx_pipeline.o ./Core/Src/mod_tx_pipeline.su ./Core/Src/modulator.cyclo ./Core/Src/modulator.d ./Core/Src/modulator.o ./Core/Src/modulator.su ./Core/Src/radio.cyclo ./Core/Src/radio.d ./Core/Src/radio.o ./Core/Src/radio.su ./Core/Src/radio_af_fit.cyclo ./Core/Src/radio_af_fit.d ./Core/Src/radio_af_fit.o ./Core/Src/radio_af_fit.su ./Core/Src/radio_af_sweep.cyclo ./Core/Src/radio_af_sweep.d ./Core/Src/radio_af_sweep.o ./Core/Src/radio_af_sweep.su ./Core/Src/radio_analog.cyclo ./Core/Src/radio_analog.d ./Core/Src/radio_analog.o ./Core/Src/radio_analog.su ./Core/Src/radio_audio.cyclo ./Core/Src/radio_audio.d ./Core/Src/radio_audio.o ./Core/Src/radio_audio.su ./Core/Src/radio_bpf_fit.cyclo ./Core/Src/radio_bpf_fit.d ./Core/Src/radio_bpf_fit.o ./Core/Src/radio_bpf_fit.su ./Core/Src/radio_bpf_sweep.cyclo ./Core/Src/radio_bpf_sweep.d ./Core/Src/radio_bpf_sweep.o ./Core/Src/radio_bpf_sweep.su ./Core/Src/radio_clocks.cyclo ./Core/Src/radio_clocks.d ./Core/Src/radio_clocks.o ./Core/Src/radio_clocks.su ./Core/Src/radio_config.cyclo ./Core/Src/radio_config.d ./Core/Src/radio_config.o ./Core/Src/radio_config.su ./Core/Src/radio_cw_decoder.cyclo ./Core/Src/radio_cw_decoder.d ./Core/Src/radio_cw_decoder.o ./Core/Src/radio_cw_decoder.su ./Core/Src/radio_digi.cyclo ./Core/Src/radio_digi.d ./Core/Src/radio_digi.o ./Core/Src/radio_digi.su ./Core/Src/radio_dsp.cyclo ./Core/Src/radio_dsp.d ./Core/Src/radio_dsp.o ./Core/Src/radio_dsp.su ./Core/Src/radio_iq_fit.cyclo ./Core/Src/radio_iq_fit.d ./Core/Src/radio_iq_fit.o ./Core/Src/radio_iq_fit.su ./Core/Src/radio_pd.cyclo ./Core/Src/radio_pd.d ./Core/Src/radio_pd.o ./Core/Src/radio_pd.su ./Core/Src/radio_pd_policy.cyclo ./Core/Src/radio_pd_policy.d ./Core/Src/radio_pd_policy.o ./Core/Src/radio_pd_policy.su ./Core/Src/radio_pd_port.cyclo ./Core/Src/radio_pd_port.d ./Core/Src/radio_pd_port.o ./Core/Src/radio_pd_port.su ./Core/Src/radio_rf_meter.cyclo ./Core/Src/radio_rf_meter.d ./Core/Src/radio_rf_meter.o ./Core/Src/radio_rf_meter.su ./Core/Src/radio_rx_cal.cyclo ./Core/Src/radio_rx_cal.d ./Core/Src/radio_rx_cal.o ./Core/Src/radio_rx_cal.su ./Core/Src/radio_rx_front.cyclo ./Core/Src/radio_rx_front.d ./Core/Src/radio_rx_front.o ./Core/Src/radio_rx_front.su ./Core/Src/radio_slot_logic.cyclo ./Core/Src/radio_slot_logic.d ./Core/Src/radio_slot_logic.o ./Core/Src/radio_slot_logic.su ./Core/Src/radio_spectrum.cyclo ./Core/Src/radio_spectrum.d ./Core/Src/radio_spectrum.o ./Core/Src/radio_spectrum.su ./Core/Src/sctrl_link.cyclo ./Core/Src/sctrl_link.d ./Core/Src/sctrl_link.o ./Core/Src/sctrl_link.su ./Core/Src/sctrl_link_logic.cyclo ./Core/Src/sctrl_link_logic.d ./Core/Src/sctrl_link_logic.o ./Core/Src/sctrl_link_logic.su ./Core/Src/sd_audio_decoder.cyclo ./Core/Src/sd_audio_decoder.d ./Core/Src/sd_audio_decoder.o ./Core/Src/sd_audio_decoder.su ./Core/Src/sd_audio_math.cyclo ./Core/Src/sd_audio_math.d ./Core/Src/sd_audio_math.o ./Core/Src/sd_audio_math.su ./Core/Src/sd_audio_player.cyclo ./Core/Src/sd_audio_player.d ./Core/Src/sd_audio_player.o ./Core/Src/sd_audio_player.su ./Core/Src/sd_diskio.cyclo ./Core/Src/sd_diskio.d ./Core/Src/sd_diskio.o ./Core/Src/sd_diskio.su ./Core/Src/sd_file_browser.cyclo ./Core/Src/sd_file_browser.d ./Core/Src/sd_file_browser.o ./Core/Src/sd_file_browser.su ./Core/Src/ui_trainer.cyclo ./Core/Src/ui_trainer.d ./Core/Src/ui_trainer.o ./Core/Src/ui_trainer.su
	-$(RM) ./Core/Src/stm32g4xx_hal_msp.cyclo ./Core/Src/stm32g4xx_hal_msp.d ./Core/Src/stm32g4xx_hal_msp.o ./Core/Src/stm32g4xx_hal_msp.su ./Core/Src/stm32g4xx_it.cyclo ./Core/Src/stm32g4xx_it.d ./Core/Src/stm32g4xx_it.o ./Core/Src/stm32g4xx_it.su ./Core/Src/syscalls.cyclo ./Core/Src/syscalls.d ./Core/Src/syscalls.o ./Core/Src/syscalls.su ./Core/Src/sysmem.cyclo ./Core/Src/sysmem.d ./Core/Src/sysmem.o ./Core/Src/sysmem.su ./Core/Src/system_stm32g4xx.cyclo ./Core/Src/system_stm32g4xx.d ./Core/Src/system_stm32g4xx.o ./Core/Src/system_stm32g4xx.su ./Core/Src/ui_app.cyclo ./Core/Src/ui_app.d ./Core/Src/ui_app.o ./Core/Src/ui_app.su ./Core/Src/ui_diag.cyclo ./Core/Src/ui_diag.d ./Core/Src/ui_diag.o ./Core/Src/ui_diag.su ./Core/Src/ui_format.cyclo ./Core/Src/ui_format.d ./Core/Src/ui_format.o ./Core/Src/ui_format.su ./Core/Src/ui_gfx.cyclo ./Core/Src/ui_gfx.d ./Core/Src/ui_gfx.o ./Core/Src/ui_gfx.su ./Core/Src/ui_help.cyclo ./Core/Src/ui_help.d ./Core/Src/ui_help.o ./Core/Src/ui_help.su ./Core/Src/ui_input.cyclo ./Core/Src/ui_input.d ./Core/Src/ui_input.o ./Core/Src/ui_input.su ./Core/Src/ui_labels.cyclo ./Core/Src/ui_labels.d ./Core/Src/ui_labels.o ./Core/Src/ui_labels.su ./Core/Src/ui_lcd.cyclo ./Core/Src/ui_lcd.d ./Core/Src/ui_lcd.o ./Core/Src/ui_lcd.su ./Core/Src/ui_menu.cyclo ./Core/Src/ui_menu.d ./Core/Src/ui_menu.o ./Core/Src/ui_menu.su ./Core/Src/ui_nav.cyclo ./Core/Src/ui_nav.d ./Core/Src/ui_nav.o ./Core/Src/ui_nav.su ./Core/Src/ui_render.cyclo ./Core/Src/ui_render.d ./Core/Src/ui_render.o ./Core/Src/ui_render.su ./Core/Src/ui_render_menu.cyclo ./Core/Src/ui_render_menu.d ./Core/Src/ui_render_menu.o ./Core/Src/ui_render_menu.su ./Core/Src/ui_render_pages.cyclo ./Core/Src/ui_render_pages.d ./Core/Src/ui_render_pages.o ./Core/Src/ui_render_pages.su ./Core/Src/ui_sd.cyclo ./Core/Src/ui_sd.d ./Core/Src/ui_sd.o ./Core/Src/ui_sd.su ./Core/Src/ui_settings_map.cyclo ./Core/Src/ui_settings_map.d ./Core/Src/ui_settings_map.o ./Core/Src/ui_settings_map.su ./Core/Src/ui_tune.cyclo ./Core/Src/ui_tune.d ./Core/Src/ui_tune.o ./Core/Src/ui_tune.su ./Core/Src/ui_waterfall.cyclo ./Core/Src/ui_waterfall.d ./Core/Src/ui_waterfall.o ./Core/Src/ui_waterfall.su ./Core/Src/usb_diag.cyclo ./Core/Src/usb_diag.d ./Core/Src/usb_diag.o ./Core/Src/usb_diag.su ./Core/Src/usb_diag_keys.cyclo ./Core/Src/usb_diag_keys.d ./Core/Src/usb_diag_keys.o ./Core/Src/usb_diag_keys.su ./Core/Src/usb_diag_mode.cyclo ./Core/Src/usb_diag_mode.d ./Core/Src/usb_diag_mode.o ./Core/Src/usb_diag_mode.su ./Core/Src/usb_diag_pins.cyclo ./Core/Src/usb_diag_pins.d ./Core/Src/usb_diag_pins.o ./Core/Src/usb_diag_pins.su ./Core/Src/usb_disk_mode.cyclo ./Core/Src/usb_disk_mode.d ./Core/Src/usb_disk_mode.o ./Core/Src/usb_disk_mode.su ./Core/Src/usbd_msc_if.cyclo ./Core/Src/usbd_msc_if.d ./Core/Src/usbd_msc_if.o ./Core/Src/usbd_msc_if.su

.PHONY: clean-Core-2f-Src

