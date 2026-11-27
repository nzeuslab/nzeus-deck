/* PC cross-compile only: extract MIPS ABI constants without running on hardware. */
#include <stdint.h>
#include <sys/ioctl.h>
#include <hcuapi/auddec.h>
#include <hcuapi/codec_id.h>
#include <hcuapi/common.h>

const uint32_t audio_abi[] = {
    sizeof(void *),
    sizeof(struct audio_config),
    AUDDEC_INIT,
    _IOW(AUDIO_IOCBASE, 1, uint8_t[632]),
    sizeof(AvPktHd),
    __builtin_offsetof(struct audio_config, extradata),
    __builtin_offsetof(struct audio_config, buffering_start),
};
