#include <avr/io.h>
#include <stdlib.h>
#include <string.h>
#include <avr/pgmspace.h>
#include <uzebox.h>
#include <spiram.h>
#include <petitfatfs/pffconf.h>
#include <petitfatfs/diskio.h>
#include <petitfatfs/pff.h>
#include "data/tiles.inc"

extern u8 ram_tiles[];
extern u8 vram[];
extern u8 mix_buf[];
extern u16 joypad1_status_lo,joypad2_status_lo,joypad1_status_hi,joypad2_status_hi;
extern volatile u8 mix_bank;
extern BYTE rcv_spi();

#define SD_SELECT()	PORTD &= ~(1<<6)
#define SD_DESELECT()	PORTD |= (1<<6)

#define VGM_BASE_ADDR  0x000400UL

#ifndef PSG_SUPPORT_SN
#define PSG_SUPPORT_SN	1
#endif
#ifndef PSG_SUPPORT_AY
#define PSG_SUPPORT_AY	1
#endif

//first write decides the active chip at runtime
#ifndef PSG_PREFER_AY_WHEN_BOTH
#define PSG_PREFER_AY_WHEN_BOTH	0
#endif

//treat N==0 as silence. (N==1 = fastest rate; do NOT force constant-1.)
#ifndef PSG_N01_MUTE
#define PSG_N01_MUTE 1
#endif

#define PSG_SR (262U*60U)

#ifndef PSG_SOFT_KNEE
#define PSG_SOFT_KNEE 0
#endif
#ifndef PSG_FILTER_DCBLOCK
#define PSG_FILTER_DCBLOCK 0
#endif
#ifndef PSG_TREBLE_SHELF
#define PSG_TREBLE_SHELF 1
#endif

#define HP_R_Q15	32760
#define LP_A_Q15	26214//~0.80 (Q15)
#define TREBLE_ATT_Q15	4915//~0.15 (Q15)

#ifndef SN_ATT_SHIFT
#define SN_ATT_SHIFT	2
#endif
#ifndef AY_ATT_SHIFT
#define AY_ATT_SHIFT	2
#endif

#define SN_DEFAULT_CLOCK_HZ	3579545u
#define AY_DEFAULT_CLOCK_HZ	1789772u

#define BUFFER_SIZE	262
#define CONT_BTN_W	16
#define CONT_BAR_X	40
#define CONT_BAR_W	(13*CONT_BTN_W)
#define CONT_BTN_H	CONT_BTN_W
#define CONT_BAR_Y	0
#define CONT_BAR_H	CONT_BTN_H

#define TILE_CURSOR	129
#define TILE_WIN_TLC	(TILE_CURSOR+1)
#define TILE_WIN_TRC	(TILE_WIN_TLC+1)
#define TILE_WIN_BLC	(TILE_WIN_TRC+3)
#define TILE_WIN_BRC	(TILE_WIN_BLC+1)
#define TILE_WIN_TBAR	(TILE_WIN_TRC+1)
#define TILE_WIN_BBAR	(TILE_WIN_TBAR+1)
#define TILE_WIN_LBAR	(TILE_WIN_BRC+1)
#define TILE_WIN_RBAR	(TILE_WIN_LBAR+1)
#define TILE_WIN_SCRU	(TILE_WIN_RBAR+1)
#define TILE_WIN_SCRD	(TILE_WIN_SCRU+1)

#define UZENET_EEPROM_ID0	32
#define UZENET_EEPROM_ID1	(UZENET_EEPROM_ID0+1)

#define PTIME_X	(SCREEN_TILES_H-9)
#define PTIME_Y	2

#define DEFAULT_COLOR_MASK	0b00000111

#define PS_LOADED	1
#define PS_PLAYING	2
#define PS_STOP		4
#define PS_PAUSE	8
#define PS_SHUFFLE	16
#define PS_DRAWN	32

static FATFS fs;
static s16 accum_span[BUFFER_SIZE];
static u8 cony = 2;
static u32 detected_ram;
static u16 oldpad = 0, pad = 0;
static u8 play_state = 0;
static u8 masterVolume = 64;
static u16 total_files = 0;
static u8 dirty_sectors = 0;
static u8 ptime_min=0, ptime_sec=0, ptime_frame=0;
static u8 g_ff_mul = 1;

static const u8 bad_masks[] PROGMEM = {
	0,1,2,3,8,9,10,11,16,17,18,19,24,25,26,27,64,65,66,67,72,73,74,75,80,81,82,83,88,89,90,91
};

typedef u8 (*psg_reader8_t)(u32 abs_addr);

typedef struct{
	u16 step_fp;

	u16 tone_N[3];
	u32 tone_cnt_fp[3];
	u8 tone_out[3];

	u8 noise_ctrl;
	u32 noise_cnt_fp;
	u32 noise_per_fp;
	u16 noise_lfsr;
	u8 noise_bit;

	u8 vol[4];
	u8 latched_chan;
	u8 latched_is_vol;

	u32 per_fp[3];
	u8 tone_mode[3];	//0=toggle, 1=force0, 2=force1
	u8 noise_mode;		//0=toggle, 1=force0, 2=force1

	s16 amp[4];		//3 tones + noise (pre-shifted)
	u8 active_mask;		//bit per channel vol!=0x0F
}sn76489_t;


typedef struct{
	u16 step_fp;
	u16 tone_N[3];
	u8 noise_N;
	u8 mixer;
	u8 vol[3];
	u8 use_env[3];

	u32 tone_cnt_fp[3];
	u8 tone_out[3];

	u32 noise_cnt_fp;
	u32 noise_per_fp;
	u32 noise_lfsr;
	u8 noise_bit;

	u32 env_per_fp;
	u32 env_cnt_fp;
	u8 env_shape;
	u8 env_level;
	u8 env_dir;
	u8 env_hold;
	u8 env_active;
	u16 env_tmp;

	u8 addr_latch;
	u32 tone_per_fp[3];
	u8 tone_mode[3];	//0=toggle, 1=force0, 2=force1
	u8 noise_mode;		//0=toggle, 1=force0, 2=force1

	s16 amp_vol[3];
	s16 amp_env_cur;
	u8 use_env_mask;
	u8 active_mask;
}ay_ym2149_t;


typedef enum{
	PSG_CHIP_NONE	= 0,
	PSG_CHIP_SN	= 1,
	PSG_CHIP_AY	= 2
}psg_chip_kind_t;

static psg_chip_kind_t g_active_chip = PSG_CHIP_NONE;

static sn76489_t g_sn;
static ay_ym2149_t g_ay;

typedef struct{
	u32 vgm_base;
	u32 vgm_size;
	u32 data_off;
	u32 loop_off;
	u32 eof_off;
	u32 pos;
	u16 wait_44k;
	u16 carry_44k;
	u8 paused;
	u8 looping;
	u16 tempo_q8_8;
	u8 mute_chip;
	psg_reader8_t rd8;
}psg_vgm_t;

static sn76489_t	g_sn;
static ay_ym2149_t	g_ay;
static psg_vgm_t	g_psg;

static u32 g_sn_clock_hz = 0;
static u32 g_ay_clock_hz = 0;
static u8 g_ay_chip_type = 0;

static const u16 kSNNoisePeriodN[3] = { 0x10u, 0x20u, 0x40u };

static const u8 kVol8[16] = {255,203,161,128,102,81,64,51,41,32,26,21,16,13,10,0};
static const u8 kAYVol_A[16] = {255,203,161,128,102,81,64,51,41,32,26,21,16,13,10,0};
static const u8 kAYVol_B[16] = {255,210,172,140,112,90,72,58,46,36,28,22,17,13,10,0};
static const u8* kAYVol = kAYVol_A;

static const u8 kAYEnvInitLevel[16] = {15,15,15,15, 0,0,0,0, 15,15,15,15, 0,0,0,0};
static const u8 kAYEnvInitDir[16] = { 0, 0, 0, 0, 1,1,1,1, 0, 0, 0, 0, 1,1,1,1};

//built once per song after AY loudness table is selected
static s16 kSN_amp_pos[16], kSN_amp_neg[16];
static s16 kAY_amp_pos[16], kAY_amp_neg[16];
static s16 kAY_env_pos[16], kAY_env_neg[16];

static inline void _psg_build_amp_tables_runtime(){
	for(u8 v=0; v<16; v++){
		s16 s = ((s16)kVol8[v] - 0x80) >> SN_ATT_SHIFT;
		kSN_amp_pos[v] = s;
		kSN_amp_neg[v] = (s16)(-s);
		s16 a = ((s16)kAYVol[v] - 0x80) >> AY_ATT_SHIFT;
		kAY_amp_pos[v] = a;
		kAY_amp_neg[v] = (s16)(-a);
		kAY_env_pos[v] = a;
		kAY_env_neg[v] = (s16)(-a);
	}
}

enum{
	VGM_CMD_END		= 0x66,
	VGM_CMD_DATA_BLOCK	= 0x67,
	VGM_CMD_WAIT_N_SAMPLES	= 0x61,
	VGM_CMD_WAIT_735	= 0x62,
	VGM_CMD_WAIT_882	= 0x63,
	VGM_CMD_WAIT_SHORT_MIN	= 0x70,
	VGM_CMD_SN_WRITE	= 0x50,
	VGM_CMD_AY0_W		= 0xA0,
	VGM_CMD_AY1_W		= 0xA1,
};

enum{
	OFF_VGM_IDENT		= 0x00,
	OFF_EOF_REL		= 0x04,
	OFF_VERSION		= 0x08,
	OFF_SN_CLOCK		= 0x0C,
	OFF_TOTAL_SAMPLES	= 0x18,
	OFF_LOOP_OFFSET_ABS	= 0x1C,
	OFF_RATE		= 0x24,
	OFF_DATA_OFFSET_REL	= 0x34,
	OFF_AY_CLOCK		= 0x74,
	OFF_AY_TYPE		= 0x78,
};

static inline void _psg_build_amp_tables_runtime();
static inline psg_vgm_t* psg_vgm_state(){ return &g_psg; }
static void psg_vgm_init_from_header(psg_vgm_t* s, u32 vgm_base, u32 vgm_size);
static void psg_vgm_pause(u8 on);
static void psg_vgm_set_ff_mul(u8 mul);
static void psg_vgm_mix_frame(u8* dst_262);
static inline u16 _psg_vgm_get16(psg_vgm_t* s);
static inline u32 _psg_vgm_get32(psg_vgm_t* s);

static void _sn_init(sn76489_t* p, u32 clock_hz);
static void _sn_write(sn76489_t* p, u8 data);
static void _sn_recalc_periods(sn76489_t* p);

static void _ay_init(ay_ym2149_t* a, u32 clock_hz);
static void _ay_write_addr(ay_ym2149_t* a, u8 addr);
static void _ay_write_data(ay_ym2149_t* a, u8 data);
static inline void _ay_recalc_noise(ay_ym2149_t* a);

static void Intro();
static void PlayerInterface();
static void DrawWindow(u8 x, u8 y, u8 w, u8 h, const char *title, const char *lb, const char *rb);
static void FileSelectWindow();
static u8 LoadDirData(u8 entry, u8 root);
static u8 SpiRamPrintString(u8 x, u8 y, u32 pos, u8 invert, u8 fill);
static void SpiRamWriteStringEntryFlash(u32 pos, const char *s);
static void SpiRamWriteStringEntry(u32 pos, char prefix, char *s);
static u16 SpiRamStringLen(u32 pos);
static void SpiRamCopyStringNoBuffer(u32 dst, u32 src, u8 max);
static void NextDir(char *s);
static void PreviousDir();
static u8 IsRootDir();
static void UMPrint(u8 x, u8 y, const char *s);
static void UMPrintRam(u8 x, u8 y, char *s);
static void UMPrintChar(u8 x, u8 y, char c);
static void PrintSongTitle(u8 x, u8 y, u8 len);
static void InputDeviceHandler();
static void UpdateCursor(u8 ylimit);
static u8 ButtonHit(u8 x, u8 y, u8 w, u8 h);

static FRESULT load_vgm(u8 reload);
static u8 start_vgm(u8 looping);
static void stop_vgm();

#define VGM_CACHE_BYTES 128
typedef struct{
	u32 base, size, pos;
	u32 buf_start;
	u16 buf_len;
	u8 buf[VGM_CACHE_BYTES];
}vgm_cache_t;

static vgm_cache_t vgmC;

static void vgm_cache_init(u32 base_addr, u32 file_size);
static void vgm_seek(u32 abs_off);
static u8 vgm_getc();

typedef struct{
	u32 base;
	u32 size;
	u32 pos;
	u32 data_off;
	u32 loop_off;
	u32 eof_off;
}VgmStream;

static VgmStream vstr;

static u8 psg_spi_read8(u32 abs_addr);
static void update_psg();
static void SilenceBuffer();
static void spi_read_block(u32 addr, u8 *dst, u16 len);