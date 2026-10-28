################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (14.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../USB_Device/App/usb_device.c \
../USB_Device/App/usbd_audio_if.c \
../USB_Device/App/usbd_cat.c \
../USB_Device/App/usbd_cdc_min.c \
../USB_Device/App/usbd_desc.c \
../USB_Device/App/usbd_diag_desc.c \
../USB_Device/App/usbd_msc_desc.c 

OBJS += \
./USB_Device/App/usb_device.o \
./USB_Device/App/usbd_audio_if.o \
./USB_Device/App/usbd_cat.o \
./USB_Device/App/usbd_cdc_min.o \
./USB_Device/App/usbd_desc.o \
./USB_Device/App/usbd_diag_desc.o \
./USB_Device/App/usbd_msc_desc.o 

C_DEPS += \
./USB_Device/App/usb_device.d \
./USB_Device/App/usbd_audio_if.d \
./USB_Device/App/usbd_cat.d \
./USB_Device/App/usbd_cdc_min.d \
./USB_Device/App/usbd_desc.d \
./USB_Device/App/usbd_diag_desc.d \
./USB_Device/App/usbd_msc_desc.d 


# Each subdirectory must supply rules for building sources it contributes
USB_Device/App/%.o USB_Device/App/%.su USB_Device/App/%.cyclo: ../USB_Device/App/%.c USB_Device/App/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DUSE_FULL_LL_DRIVER -DUSBPD_PORT_COUNT=1 -D_SNK -DUSBPDCORE_LIB_PD3_CONFIG_1 -DDEBUG -DUSE_HAL_DRIVER -DSTM32G474xx -c -I../Middlewares/ST/STM32_USBPD_Library/Core/inc -I../Middlewares/ST/STM32_USBPD_Library/Devices/STM32G4XX/inc -I../Core/Inc -I../Drivers/STM32G4xx_HAL_Driver/Inc -I../Drivers/STM32G4xx_HAL_Driver/Inc/Legacy -I../Drivers/CMSIS/Device/ST/STM32G4xx/Include -I../Drivers/CMSIS/Include -I../USB_Device/App -I../USB_Device/Target -I../Middlewares/ST/STM32_USB_Device_Library/Core/Inc -I../Middlewares/ST/STM32_USB_Device_Library/Class/AUDIO/Inc -I../Middlewares/ST/STM32_USB_Device_Library/Class/MSC/Inc -O2 -ffunction-sections -fdata-sections -Wall -fno-math-errno -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-USB_Device-2f-App

clean-USB_Device-2f-App:
	-$(RM) ./USB_Device/App/usb_device.cyclo ./USB_Device/App/usb_device.d ./USB_Device/App/usb_device.o ./USB_Device/App/usb_device.su ./USB_Device/App/usbd_audio_if.cyclo ./USB_Device/App/usbd_audio_if.d ./USB_Device/App/usbd_audio_if.o ./USB_Device/App/usbd_audio_if.su ./USB_Device/App/usbd_cat.cyclo ./USB_Device/App/usbd_cat.d ./USB_Device/App/usbd_cat.o ./USB_Device/App/usbd_cat.su ./USB_Device/App/usbd_cdc_min.cyclo ./USB_Device/App/usbd_cdc_min.d ./USB_Device/App/usbd_cdc_min.o ./USB_Device/App/usbd_cdc_min.su ./USB_Device/App/usbd_desc.cyclo ./USB_Device/App/usbd_desc.d ./USB_Device/App/usbd_desc.o ./USB_Device/App/usbd_desc.su ./USB_Device/App/usbd_diag_desc.cyclo ./USB_Device/App/usbd_diag_desc.d ./USB_Device/App/usbd_diag_desc.o ./USB_Device/App/usbd_diag_desc.su ./USB_Device/App/usbd_msc_desc.cyclo ./USB_Device/App/usbd_msc_desc.d ./USB_Device/App/usbd_msc_desc.o ./USB_Device/App/usbd_msc_desc.su

.PHONY: clean-USB_Device-2f-App

