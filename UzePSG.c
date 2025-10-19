#include "UzePSG.h"

static inline s8 _sat8(s32 x){
	if(x > 127)
		return 127;
	if(x < -128)
		return -128;
	return (s8)x;
}


static void SilenceBuffer(void){
	for(u16 i=0;i<262u*2u;i++)
		mix_buf[i]=0x80;
}


static u8 psg_spi_read8(u32 abs_addr){
	return SpiRamReadU8((u8)(abs_addr>>16), (u16)(abs_addr&0xFFFF));
}


static void spi_read_block(u32 addr, u8 *dst, u16 len){
	while(len){
		u8  bank = (u8)(addr>>16);
		u16 off  = (u16)(addr&0xFFFF);
		u32 room = 0x10000UL - (u32)off;
		u16 chunk= (u16)((room < len) ? room : len);
		SpiRamReadInto(bank, off, dst, chunk);
		addr += chunk; dst += chunk; len -= chunk;
	}
}


static void vgm_cache_init(u32 base_addr, u32 file_size){
	vgmC.base = base_addr;
	vgmC.size = file_size;
	vgmC.pos = 0;
	vgmC.buf_start = 0;
	vgmC.buf_len = 0;
}


static void vgm_seek(u32 abs_off){
	if(abs_off >= vgmC.size)
		abs_off = vgmC.size ? (vgmC.size-1) : 0;
	vgmC.pos = abs_off;
	vgmC.buf_len = 0;
	vstr.pos = abs_off;
}


static u8 vgm_getc(){
	if(!(vgmC.pos >= vgmC.buf_start && vgmC.pos < (vgmC.buf_start + vgmC.buf_len))){
		vgmC.buf_start = vgmC.pos;
		u16 want = (u16)((vgmC.size - vgmC.buf_start) > VGM_CACHE_BYTES ? VGM_CACHE_BYTES : (vgmC.size - vgmC.buf_start));
		if(want == 0)
			want = 1;
		spi_read_block(vgmC.base + vgmC.buf_start, vgmC.buf, want);
		vgmC.buf_len = want;
	}
	u8 b = vgmC.buf[vgmC.pos - vgmC.buf_start];
	vgmC.pos++; vstr.pos = vgmC.pos;
	return b;
}


void _psg_defaults(psg_vgm_t *s){
	memset(s,0,sizeof(*s));
	s->tempo_q8_8 = 0x0100;
	s->rd8        = psg_spi_read8;
}


static inline u8  _rd8(psg_vgm_t* s, u32 abs){
	return s->rd8? s->rd8(abs):0;
}


static inline u32 _rd32(psg_vgm_t* s, u32 abs){
	u32 b0 = _rd8(s,abs+0), b1 = _rd8(s,abs+1), b2 = _rd8(s,abs+2), b3 = _rd8(s,abs+3);
	return b0 | (b1<<8) | (b2<<16) | (b3<<24);
}


void psg_vgm_pause(u8 on){
	g_psg.paused = on ? 1 : 0;
}


void psg_vgm_set_ff_mul(u8 mul){
	g_ff_mul = mul ? mul : 1;
}


static inline void _sn_step_lfsr(sn76489_t *p){
	u16 fb;
	/* bit2: 0=periodic, 1=white */
	if(p->noise_ctrl & 0x04)
		fb = (u16)((p->noise_lfsr ^ (p->noise_lfsr >> 3)) & 1u);
	else
		fb = (u16)(p->noise_lfsr & 1u);
	p->noise_lfsr = (u16)((p->noise_lfsr >> 1) | (fb << 14));
	p->noise_bit  = (u8)(p->noise_lfsr & 1u);
}

static inline u8  _sn_chan_from_latch(u8 d){
	return (u8)((d>>5)&3);
}

static void _sn_recalc_periods(sn76489_t* p){
	for(u8 i=0; i<3; i++){
		u16 N = (u16)(p->tone_N[i] & 0x03FF);
#if PSG_N01_MUTE
		p->per_fp[i] = (N==0)?0 : ((u32)N<<11);
#else
		p->per_fp[i] = (N==0)?1 : ((u32)N<<11);
#endif
	}
	u8 r = (u8)(p->noise_ctrl & 3);
	if(r==3)
		p->noise_per_fp = p->per_fp[2];
	else{
		u32 N = kSNNoisePeriodN[r];
#if PSG_N01_MUTE
		p->noise_per_fp = ((u32)N<<11);//N==0 never happens here
#else
		p->noise_per_fp = (N==0)?1:((u32)N<<11);
#endif
	}
}


static void _sn_init(sn76489_t *p, u32 clock_hz){
	u32 step = ((clock_hz/16u) + (PSG_SR>>1))/PSG_SR;
	p->step_fp = (u16)(step<<8);
	for(u8 i=0; i<3; i++){
		p->tone_N[i]=0;
		p->tone_cnt_fp[i]=0;
		p->tone_out[i]=0;
		p->per_fp[i]=1;
	}
	p->noise_ctrl = 0;
	p->noise_cnt_fp = 0;
	p->noise_per_fp = 1;
	p->noise_lfsr = 0x4000u;
	p->noise_bit = 1;
	p->vol[0] = p->vol[1] = p->vol[2] = p->vol[3] = 15;
	p->latched_chan = 0;
	p->latched_is_vol = 0;
	_sn_recalc_periods(p);
}

static void _sn_write(sn76489_t *p, u8 data){
	if(data & 0x80){
		p->latched_chan = _sn_chan_from_latch(data);
		p->latched_is_vol = (u8)((data>>4)&1);
		u8 low4 = (u8)(data & 0x0F);
		if(p->latched_is_vol){
			p->vol[p->latched_chan] = (u8)(low4 & 0x0F);
		}else{
			if(p->latched_chan < 3){
				p->tone_N[p->latched_chan] = (u16)((p->tone_N[p->latched_chan] & 0x03F0) | low4);
				_sn_recalc_periods(p);
			}else{
				p->noise_ctrl = (u8)(low4 & 0x0F);
				_sn_recalc_periods(p);
			}
		}
	}else{
		if(p->latched_is_vol){
			p->vol[p->latched_chan] = (u8)(data & 0x0F);
		}else{
			if(p->latched_chan < 3){
				u16 hi6 = (u16)((data & 0x3F) << 4);
				p->tone_N[p->latched_chan] = (u16)((p->tone_N[p->latched_chan] & 0x000F) | hi6);
				_sn_recalc_periods(p);
			}else{
				p->noise_ctrl = (u8)(data & 0x0F);
				_sn_recalc_periods(p);
			}
		}
	}
}


static inline u32 _ay_per_tone_fp(u16 N12){
	N12 &= 0x0FFF;
#if PSG_N01_MUTE
	if(N12==0)
		return 0;
#else
	if(N12==0)
		return 1;
#endif
	return (u32)N12 << 11;
}
static inline u32 _ay_per_noise_fp(u8 n5){
	n5 &= 0x1F;
#if PSG_N01_MUTE
	if(n5==0)
		return 0;
#else
	if(n5==0)
		return 1;
#endif
	return (u32)n5 << 11;
}


static inline void _ay_env_restart(ay_ym2149_t *a){
	u8 sh = (u8)(a->env_shape & 0x0F);
	a->env_dir = kAYEnvInitDir[sh];
	a->env_hold = (u8)((sh & 0x01)?1:0);
	a->env_level = kAYEnvInitLevel[sh];
	a->env_active = 1;
	a->env_cnt_fp = 0;
}


static void _ay_init(ay_ym2149_t *a, u32 clock_hz){
	u32 step = (clock_hz + (PSG_SR>>1))/PSG_SR;
	a->step_fp = (u16)(step<<8);
	for(u8 i=0; i<3; i++){
		a->tone_N[i] = 0;
		a->tone_cnt_fp[i] = 0;
		a->tone_out[i] = 0;
		a->tone_per_fp[i] = 1;
	}
	a->noise_N = 0;
	a->mixer = 0xFF;
	a->noise_lfsr = 1;
	a->noise_cnt_fp = 0;
	a->noise_per_fp = 1;
	a->noise_bit = 1;

	a->env_per_fp = 1;
	a->env_cnt_fp = 0;
	a->env_shape = 0;
	a->env_level = 0;
	a->env_dir = 0;
	a->env_hold = 0;
	a->env_active = 0;
	a->env_tmp = 0;
	a->addr_latch = 0;
	for(u8 i=0; i<3; i++){
		a->vol[i] = 0;
		a->use_env[i] = 0;
	}
	_ay_recalc_noise(a);
}


static inline void _ay_recalc_noise(ay_ym2149_t *a){
	a->noise_per_fp = _ay_per_noise_fp(a->noise_N);
}


static void _ay_write_addr(ay_ym2149_t *a, u8 addr){
	a->addr_latch=(u8)(addr&0x0F);
}


static void _ay_write_data(ay_ym2149_t *a, u8 data){
	switch(a->addr_latch){
		case 0: case 2: case 4:{
			u8 ch = a->addr_latch>>1;
			a->tone_N[ch] = (u16)((a->tone_N[ch]&0x0F00)|data);
			a->tone_per_fp[ch] = _ay_per_tone_fp(a->tone_N[ch]);
		}break;
		case 1: case 3: case 5:{
			u8 ch = (a->addr_latch-1)>>1;
			a->tone_N[ch] = (u16)((a->tone_N[ch]&0x00FF)|((u16)(data&0x0F)<<8));
			a->tone_per_fp[ch] = _ay_per_tone_fp(a->tone_N[ch]);
		}break;
		case 6:{
			a->noise_N = (u8)(data&0x1F);
			_ay_recalc_noise(a);
		}break;
		case 7:{
			a->mixer=data;
		}break;
		case 8: case 9: case 10:{
			u8 ch = a->addr_latch-8;
			a->use_env[ch] = (u8)((data>>4)&1);
			a->vol[ch] = (u8)(data&0x0F);
		}break;
		case 11: case 12:{
			if(a->addr_latch == 11)
				a->env_tmp = (u16)((a->env_tmp&0xFF00)|data);
			else{
				a->env_tmp = (u16)((a->env_tmp&0x00FF)|((u16)data<<8));
				a->env_per_fp = ((u32)a->env_tmp << 11) << 5;//16× tone scale
			}
		}break;
		case 13:{
			a->env_shape = (u8)(data&0x0F);
			_ay_env_restart(a);
		}break;
		default:
			break;
	}
}


void psg_vgm_init_from_header(psg_vgm_t *s, u32 vgm_base, u32 vgm_size){
	_psg_defaults(s);

	s->vgm_base = vgm_base;
	s->vgm_size = vgm_size;

	u32 eof_rel   = _rd32(s, vgm_base + OFF_EOF_REL);
	s->eof_off    = eof_rel ? (0x04 + eof_rel) : vgm_size;

	u32 data_rel  = _rd32(s, vgm_base + OFF_DATA_OFFSET_REL);
	s->data_off   = data_rel ? (0x34 + data_rel) : 0x40;

	u32 loop_rel  = _rd32(s, vgm_base + OFF_LOOP_OFFSET_ABS);
	s->loop_off   = loop_rel ? (0x1C + loop_rel) : 0;
	s->looping    = (s->loop_off != 0);
	s->pos        = s->data_off;

	u32 snc = _rd32(s, vgm_base + OFF_SN_CLOCK);
	u32 ayc = _rd32(s, vgm_base + OFF_AY_CLOCK);

	g_sn_clock_hz = (snc & 0x3FFFFFFFu);
	if(!g_sn_clock_hz)
		g_sn_clock_hz = SN_DEFAULT_CLOCK_HZ;
	g_ay_clock_hz = (ayc & 0x3FFFFFFFu);
	if(!g_ay_clock_hz)
		g_ay_clock_hz = AY_DEFAULT_CLOCK_HZ;
	g_ay_chip_type= _rd8(s, vgm_base + OFF_AY_TYPE);

	//AY loudness table selection + (re)build mixed LUTs
	kAYVol = ((g_ay_chip_type & 0x03) == 1) ? kAYVol_B : kAYVol_A;
	_psg_build_amp_tables_runtime();

	//init both, first write determines which we will use
	_sn_init(&g_sn, g_sn_clock_hz);
	_ay_init(&g_ay, g_ay_clock_hz);

	g_active_chip = PSG_CHIP_NONE;
}


static inline void _psg_vgm_seek(psg_vgm_t *s, u32 rel_off){
	if(rel_off >= s->vgm_size)
		rel_off = s->vgm_size ? (s->vgm_size - 1) : 0;
	vgm_seek(rel_off);
	s->pos = rel_off;
}


static inline u8 _psg_vgm_getc(psg_vgm_t *s){
	u8 b = vgm_getc();
	s->pos = vgmC.pos;
	return b;
}


static inline u16 _psg_vgm_get16(psg_vgm_t *s){
	u16 lo = _psg_vgm_getc(s);
	return (u16)(lo | ((u16)_psg_vgm_getc(s)<<8));
}


static inline u32 _psg_vgm_get32(psg_vgm_t *s){
	u32 b0 = _psg_vgm_getc(s), b1 = _psg_vgm_getc(s), b2 = _psg_vgm_getc(s), b3 = _psg_vgm_getc(s);
	return b0 | (b1<<8) | (b2<<16) | (b3<<24);
}


static inline void _psg_apply_wait(psg_vgm_t *s, u16 w44){
	u32 scaled = ((u32)w44 * (u32)s->tempo_q8_8) >> 8;
	u32 num    = s->carry_44k + scaled * (u32)PSG_SR;
	u16 n      = (u16)(num / 44100u);
	s->carry_44k = (u16)(num % 44100u);
	if(n == 0)
		n = 1;
	s->wait_44k = n;
}


static u8 _psg_parse_until_wait(psg_vgm_t *s){
	if(s->pos >= s->eof_off){
		if(s->looping && s->loop_off){
			_psg_vgm_seek(s, s->loop_off);
		}else{
			s->paused=1;
		}
		return 0;
	}
	while(1){
		if(s->pos >= s->eof_off){
			if(s->looping && s->loop_off){
				_psg_vgm_seek(s, s->loop_off);
			}else{
				s->paused=1;
			}
			return 0;
		}
		u8 c = _psg_vgm_getc(s);

		if(c == VGM_CMD_WAIT_N_SAMPLES){
			_psg_apply_wait(s, _psg_vgm_get16(s));
			return 1;
		}
		if(c == VGM_CMD_WAIT_735){\
			_psg_apply_wait(s, 735);
			return 1;
		}
		if(c == VGM_CMD_WAIT_882){
			_psg_apply_wait(s, 882);
			return 1;
		}
		if(c >= VGM_CMD_WAIT_SHORT_MIN && c<=0x7F){
			_psg_apply_wait(s, (u16)((c&0x0F)+1));
			return 1;
		}

		if(c == VGM_CMD_END){
			if(s->looping && s->loop_off){
				_psg_vgm_seek(s, s->loop_off);
				continue;
			}
			s->paused=1;
			return 0;
		}

		if(c == VGM_CMD_DATA_BLOCK){
			(void)_psg_vgm_getc(s);
			u8 type = _psg_vgm_getc(s);
			u32 size= _psg_vgm_get32(s);
			(void)type;
			_psg_vgm_seek(s, s->pos + size);
			continue;
		}

		if(c == VGM_CMD_SN_WRITE){
			u8 d = _psg_vgm_getc(s);
			if(g_active_chip == PSG_CHIP_NONE)
				g_active_chip = PSG_CHIP_SN;
			if(g_active_chip == PSG_CHIP_SN){
				_sn_write(&g_sn, d);
			}
			continue;
		}

		if(c == VGM_CMD_AY0_W || c == VGM_CMD_AY1_W){
			u8 a = _psg_vgm_getc(s), v = _psg_vgm_getc(s);
			if(g_active_chip==PSG_CHIP_NONE)
				g_active_chip = PSG_CHIP_AY;
			if(g_active_chip == PSG_CHIP_AY){
				_ay_write_addr(&g_ay, a);
				_ay_write_data(&g_ay, v);
			}
			continue;
		}

		//skip unknowns for other chips
		if((c >= 0x51 && c <= 0x5F) || (c >= 0xA2 && c <= 0xAF) || (c >= 0xB0 && c <= 0xBF)){
			_psg_vgm_seek(s, s->pos+2);
			continue;
		}
		if(c >= 0xC0 && c <= 0xCF){
			_psg_vgm_seek(s, s->pos+3);
			continue;
		}
	}
}


static void _mix_sn_chunk(u8 *outp, u16 count){
	sn76489_t *sn = &g_sn;

	for(u16 i=0; i<count; i++){
		s32 chip = 0;

		for(u8 ch=0; ch<3; ch++){
			u32 per = sn->per_fp[ch];

#if PSG_N01_MUTE
			if(per==0){
				sn->tone_out[ch]=0;
				sn->tone_cnt_fp[ch]=0;
			}
#else
			if(per==1){
				sn->tone_out[ch]=1;
				sn->tone_cnt_fp[ch]=0;
			}else if(per==0){
				sn->tone_out[ch]=0;
				sn->tone_cnt_fp[ch]=0;
			}
#endif
			else{
				sn->tone_cnt_fp[ch]+=sn->step_fp;
				if(sn->tone_cnt_fp[ch]>=per){
					sn->tone_cnt_fp[ch] -= per;
					sn->tone_out[ch] ^= 1;
				}
			}

			u8 v = sn->vol[ch] & 0x0F;
			if(v!=0x0F){//unipolar SN
				chip += sn->tone_out[ch] ? kSN_amp_pos[v] : 0;
			}
		}

		u32 nper = sn->noise_per_fp;
#if PSG_N01_MUTE
		if(nper==0){
			sn->noise_cnt_fp=0;
			sn->noise_bit=0;
		}
#else
		if(nper==1){//max rate(no “stuck 1”)
			sn->noise_cnt_fp=0;
			_sn_step_lfsr(sn);
		}else if(nper==0){
			sn->noise_cnt_fp=0;
			sn->noise_bit=0;
		}
#endif
		else{
			sn->noise_cnt_fp += sn->step_fp;
			if(sn->noise_cnt_fp>=nper){
				sn->noise_cnt_fp -= nper;
				_sn_step_lfsr(sn);
			}
		}
		{
			u8 v = sn->vol[3] & 0x0F;
			if(v!=0x0F){
				chip += sn->noise_bit ? kSN_amp_pos[v] : 0;
			}
		}

		outp[i] = (u8)(_sat8(chip) + 0x80);
	}
}


static void _ay_env_tick(ay_ym2149_t *a){
	if(!a->env_active)
		return;
	a->env_cnt_fp += a->step_fp;
	if(a->env_cnt_fp >= a->env_per_fp && a->env_per_fp){
		a->env_cnt_fp -= a->env_per_fp;
		if(a->env_dir){
			if(a->env_level < 15){
				a->env_level++;
			}else{
				if(a->env_hold){
					a->env_active=0;
				}else{
					if(a->env_shape & 0x02){
						a->env_dir = 0;
						a->env_level = 14;
					}else
						a->env_level = 0;
				}
			}
		}else{
			if(a->env_level > 0)
				a->env_level--;
			else{
				if(a->env_hold){
					a->env_active=0;
				}else{
					if(a->env_shape & 0x02){
						a->env_dir = 1;
						a->env_level = 1;
					}else
						a->env_level = 15;
				}
			}
		}
	}
}


static void _mix_ay_chunk(u8 *outp, u16 count){
	ay_ym2149_t *ay = &g_ay;

	for(u16 i=0; i<count; i++){
		s32 chip = 0;

		_ay_env_tick(ay);
		u8 envL = ay->env_level;

		for(u8 ch=0; ch<3; ch++){//tone flip-flops
			u32 per = ay->tone_per_fp[ch];

#if PSG_N01_MUTE
			if(per == 0){
				ay->tone_out[ch]=0;
				ay->tone_cnt_fp[ch]=0;
			}
#else
			if(per == 1){
				ay->tone_out[ch]=1;
				ay->tone_cnt_fp[ch]=0;
			}else if(per == 0){
				ay->tone_out[ch]=0;
				ay->tone_cnt_fp[ch]=0;
			}
#endif
			else{
				ay->tone_cnt_fp[ch] += ay->step_fp;
				if(ay->tone_cnt_fp[ch] >= per){
					ay->tone_cnt_fp[ch] -= per;
					ay->tone_out[ch] ^= 1;
				}
			}
		}

		u32 nper2 = ay->noise_per_fp;
#if PSG_N01_MUTE
		if(nper2 == 0){
			ay->noise_cnt_fp = 0;
			ay->noise_bit = 0;
		}
#else
		if(nper2 == 1){//max-rate noise: step every sample (don’t stick high)
			u32 fb = ((ay->noise_lfsr ^ (ay->noise_lfsr>>3))&1);
			ay->noise_lfsr = (ay->noise_lfsr>>1)|(fb<<16);
			ay->noise_bit = (u8)(ay->noise_lfsr&1);
			ay->noise_cnt_fp = 0;
		}else if(nper2 == 0){
			ay->noise_cnt_fp = 0;
			ay->noise_bit = 0;
		}
#endif
		else{
			ay->noise_cnt_fp += ay->step_fp;
			if(ay->noise_cnt_fp >= nper2){
				ay->noise_cnt_fp -= nper2;
				u32 fb = ((ay->noise_lfsr ^ (ay->noise_lfsr>>3))&1);
				ay->noise_lfsr = (ay->noise_lfsr>>1)|(fb<<16);
				ay->noise_bit = (u8)(ay->noise_lfsr&1);
			}
		}

		for(u8 ch=0; ch<3; ch++){//per channel add
			u8 tone_en  = (u8)((ay->mixer & (1u<<ch)) == 0);
			u8 noise_en = (u8)((ay->mixer & (0x08u<<ch)) == 0);
			u8 gate = (u8)((tone_en ? ay->tone_out[ch] : 1) & (noise_en ? ay->noise_bit : 1));
			if(!gate)
				continue;

			u8 use_env = ay->use_env[ch];
			if(use_env){
				chip += kAY_env_pos[envL];
			}else{
				u8 v = ay->vol[ch] & 0x0F;
				if(v!=0x0F)
					chip += kAY_amp_pos[v];
			}
		}
		outp[i] = (u8)(_sat8(chip) + 0x80);
	}
}


static void psg_vgm_mix_frame(u8 *dst_262){
	psg_vgm_t *s = &g_psg;
	if(s->paused){
		for(u16 i=0; i<262; i++)
			dst_262[i] = 0x80;
		return;
	}

	u16 remain = 262;
	while(remain){
		if(s->wait_44k==0){
			if(!_psg_parse_until_wait(s)){
				u8 *p = dst_262 + (262 - remain);
				for(u16 j=0; j<remain; j++)
					p[j] = 0x80;
				return;
			}
		}

		u16 until = s->wait_44k;
		u16 chunk = until / g_ff_mul;
		if(chunk == 0)
			chunk=1;
		if(chunk > remain)
			chunk = remain;

		u8 *outp = dst_262 + (262 - remain);

		switch(g_active_chip){
			case PSG_CHIP_SN:
				_mix_sn_chunk(outp, chunk);
			break;
			case PSG_CHIP_AY:
				_mix_ay_chunk(outp, chunk);
			break;
			default:
				for(u16 i=0;i<chunk;i++)
					outp[i]=0x80;
			break;
		}

		u32 sub = (u32)chunk * (u32)g_ff_mul;
		s->wait_44k = (s->wait_44k > sub) ? (u16)(s->wait_44k - sub) : 0;
		remain -= chunk;
	}
}


static FRESULT load_vgm(u8 reload){
	(void)reload;
	u8 *as8 = (u8 *)accum_span;
	ptime_min = ptime_sec = ptime_frame = 0;

	SetRenderingParameters(33, 10*8);
	cony = 1;
	DrawWindow(1, 0, 28,8,NULL,NULL,NULL);
	SpiRamReadInto(0,0,as8,32);

	FRESULT fr = pf_open((const char *)as8);
	if(fr)
		return fr;

	while(rcv_spi()!=0xFF);
	SD_DESELECT();

	u32 addr  = VGM_BASE_ADDR;
	u8  bank  = (u8)(addr >> 16);
	u16 off16 = (u16)(addr & 0xFFFF);

	UINT br;
	u8 buf[128];
	u32 vgm_size = 0;

	while(1){
		fr = pf_read(buf, sizeof(buf), &br);
		while(rcv_spi()!=0xFF);
		SD_DESELECT();
		if(fr)
			return fr;
		if(br==0)
			break;

		SpiRamSeqWriteStart(bank, off16);
		for(u16 i=0; i<br; i++){
			SpiRamSeqWriteU8(buf[i]);
			if(++off16==0)
				bank++;
		}
		SpiRamSeqWriteEnd();
		vgm_size += br;
	}

	vgm_cache_init(VGM_BASE_ADDR, vgm_size);

	vstr.base = VGM_BASE_ADDR;
	vstr.size = vgm_size;
	vstr.eof_off = vgm_size;
	vstr.data_off= 0;
	vstr.loop_off= 0;
	vstr.pos     = 0;

	return FR_OK;
}


static u8 start_vgm(u8 looping){
	(void)looping;
	SilenceBuffer();
	g_psg.paused = 0;

	psg_vgm_init_from_header(&g_psg, VGM_BASE_ADDR, vgmC.size);

	vstr.base     = g_psg.vgm_base;
	vstr.size     = g_psg.vgm_size;
	vstr.data_off = g_psg.data_off;
	vstr.loop_off = g_psg.loop_off;
	vstr.eof_off  = g_psg.eof_off;
	vstr.pos      = g_psg.data_off;

	vgm_seek(vstr.data_off);
	g_psg.wait_44k = 0;
	g_psg.carry_44k = 0;

	g_active_chip = PSG_CHIP_NONE;//one will be activated on first write

	for(u16 i=0; g_psg.wait_44k == 0 && !g_psg.paused && i<4096; i++){
		if(!_psg_parse_until_wait(&g_psg))
			break;
	}
	if(g_psg.wait_44k==0)
		g_psg.wait_44k = 1;

	return 0;
}


static void stop_vgm(void){
	SilenceBuffer();
	g_psg.paused = 1;
}


static void Intro(){
	SetFontTilesIndex(0);
	SetTileTable(tile_data);
	SetSpritesTileTable(tile_data);
	return;
}


static void UMPrintChar(u8 x, u8 y, char c){
	if(c>='a') c-=32;
	SetTile(x,y,(u8)(c-32));
}


static void UMPrint(u8 x, u8 y, const char *s){
	u8 i = 0;
	while(1){
		char c = pgm_read_byte(&s[i++]);
		if(!c)
			break;
		UMPrintChar(x++,y,c);
	}
}


static void UMPrintRam(u8 x, u8 y, char *s){
	u8 i = 0;
	while(1){
		char c = s[i++];
		if(!c)
			break;
		UMPrintChar(x++,y,c);
	}
}


static void DrawWindow(u8 x, u8 y, u8 w, u8 h, const char *title, const char *lb, const char *rb){
	SetTile(x+0,y+0,TILE_WIN_TLC);
	SetTile(x+w,y+0,TILE_WIN_TRC);
	SetTile(x+0,y+h,TILE_WIN_BLC);
	SetTile(x+w,y+h,TILE_WIN_BRC);
	for(u8 y2=y+1; y2<y+h; y2++){
		for(u8 x2=x+1; x2<(x+w); x2++){
			SetTile(x2,y2,0);
		}
	}
	for(u8 x2=x+1; x2<x+w; x2++){
		SetTile(x2,y,TILE_WIN_TBAR);
		SetTile(x2,y+h,TILE_WIN_BBAR);
	}
	for(u8 y2=y+1; y2<y+h; y2++){
		SetTile(x,y2,TILE_WIN_LBAR);
		SetTile(x+w,y2,TILE_WIN_RBAR);
	}
	if(title)
		UMPrint(x+1,y,title);
	if(lb)
		UMPrint(x+1,y+h,lb);
	if(rb){
		u8 xo = x+w;
		for(u8 i=0;i<16;i++){
			if(pgm_read_byte(&rb[i])=='\0')
				break; xo--;
		}
		UMPrint(xo,y+h,rb);
	}
}


static u16 SpiRamStringLen(u32 pos){
	SpiRamSeqReadStart((u8)(pos>>16),(u16)(pos&0xFFFF));
	u16 len=0;
	while(SpiRamSeqReadU8()!=0 && len<4096)
		len++;
	SpiRamSeqReadEnd();
	return len;
}


static void SpiRamWriteStringEntryFlash(u32 pos, const char *s){
	SpiRamSeqWriteStart((u8)(pos>>16),(u16)(pos&0xFFFF));
	u8 i;
	for(i=0; i<32; i++){
		char c=pgm_read_byte(&s[i]);
		SpiRamSeqWriteU8(c);
		if(!c)
			break;
	}
	for(;	i<32;	i++)
		SpiRamSeqWriteU8('\0');
	SpiRamSeqWriteEnd();
}


static void SpiRamWriteStringEntry(u32 pos, char prefix, char *s){
	SpiRamSeqWriteStart((u8)(pos>>16),(u16)(pos&0xFFFF));
	if(prefix)
		SpiRamSeqWriteU8(prefix);
	u8 i;
	for(i=0; i<32; i++){
		char c=*s++;
		SpiRamSeqWriteU8(c);
		if(!c)
			break;
	}
	for(; i<32; i++)
		SpiRamSeqWriteU8('\0');
	SpiRamSeqWriteEnd();
}


static u8 SpiRamPrintString(u8 x, u8 y, u32 pos, u8 invert, u8 fill){
	SpiRamSeqReadStart((u8)(pos>>16),(u16)(pos&0xFFFF));
	u8 ret=255;
	while(1){
		char c = SpiRamSeqReadU8();
		if(ret==255)
			ret = (c=='/')?1:0;
		if(c=='\0'){
			while(fill){
				SetTile(x++,y,0);
				fill--;
			}
			break;
		}
		if(invert)
			c+=64;
		if(fill)
			fill--;
		UMPrintChar(x++,y,c);
	}
	SpiRamSeqReadEnd();
	return ret;
}


static u8 IsRootDir(){
	u32 base = (u32)(detected_ram-512)+1;
	u8 c = SpiRamReadU8((u8)(base>>16),(u16)(base&0xFFFF));
	return (c==0)?1:0;
}


void PreviousDir(){
	u32 base = (u32)(detected_ram-512);
	u16 slen = SpiRamStringLen(base);
	u32 last_slash = 0;
	SpiRamSeqReadStart((u8)(base>>16),(u16)(base&0xFFFF));
	for(u16 i=0;i<slen-1;i++){
		if(SpiRamSeqReadU8()=='/')
			last_slash=i;
	}
	SpiRamSeqReadEnd();
	if(last_slash==0)
		last_slash = 1;
	base += (u32)last_slash;
	SpiRamWriteU8((u8)(base>>16),(u16)(base&0xFFFF),0);
}


void NextDir(char *s){
	u32 base = (u32)(detected_ram-512);
	if(!s){
		SpiRamWriteStringEntryFlash(base, PSTR("/"));
		return;
	}
	base += (u32)SpiRamStringLen(base);
	u8 isroot = IsRootDir();
	SpiRamSeqWriteStart((u8)(base>>16),(u16)(base&0xFFFF));
	if(!isroot)
		SpiRamSeqWriteU8('/');
	while(*s){
		SpiRamSeqWriteU8(*s++);
	}
	SpiRamSeqWriteEnd();
}


void SpiRamCopyStringNoBuffer(u32 dst, u32 src, u8 max){
	while(max--){
		char c = SpiRamReadU8((u8)(src>>16), (u16)(src&0xFFFF));
		SpiRamWriteU8((u8)(dst>>16), (u16)(dst&0xFFFF), c);
		if(!c)
			return;
		src++;
		dst++;
	}
	dst--;
	SpiRamWriteU8((u8)(dst>>16), (u16)(dst&0xFFFF), '\0');
}


u8 LoadDirData(u8 entry, u8 root){
	(void)root;
	total_files = 0;
	u8 *as8=(u8*)accum_span;

	SpiRamReadInto(0,0,as8,64);
	u32 base = (u32)(detected_ram-64);
	SpiRamWriteFrom((u8)(base>>16),(u16)(base&0xFFFF),as8,64);

	base = (u32)(entry*32UL);
	SpiRamReadInto((u8)(base>>16),(u16)(base&0xFFFF),as8,32);
	base = (u32)(detected_ram-512);
	SpiRamReadInto((u8)(base>>16),(u16)(base&0xFFFF),as8,256);

	FRESULT res;
	FILINFO fno; DIR dir;
	res = pf_opendir(&dir, (const char*)as8);
	if(res==FR_OK){
		while(1){
			res = pf_readdir(&dir, &fno);
			if(res!=FR_OK || fno.fname[0]==0)
				break;

			if((fno.fattrib & AM_DIR)){
				SpiRamWriteStringEntry((u32)(total_files*32), '/', fno.fname);
				total_files++;
			}else{
				u8 valid=0;
				for(u8 i=0; i<10; i++){
					if(fno.fname[i]==0)
						break;
					if(fno.fname[i] == '.' && (fno.fname[i+1] == 'V') && (fno.fname[i+2] == 'G') && (fno.fname[i+3] == 'M' || fno.fname[i+3] == 'Z')){
						valid=1;
						break;
					}
				}
				if(valid){
					SpiRamWriteStringEntry((u32)(total_files*32), 0, fno.fname);
					total_files++;
				}
			}
		}
		dirty_sectors = 1+((total_files*64)/512);
		return 0;
	}
	dirty_sectors = 1+((total_files*64)/512);
	return res;
}


u8 ButtonHit(u8 x, u8 y, u8 w, u8 h){
	if(sprites[0].x < (x<<3) || sprites[0].x > (x<<3)+(w<<3) || sprites[0].y < (y<<3) || sprites[0].y > (y<<3)+(h<<3))
		return 0;
	return 1;
}


#define Wait200ns() asm volatile("lpm\n\tlpm\n\t")
static void InputDeviceHandler(){
	u8 i;
	joypad1_status_lo = joypad2_status_lo = joypad1_status_hi = joypad2_status_hi = 0;

	JOYPAD_OUT_PORT |= _BV(JOYPAD_LATCH_PIN);
	for(i=0; i<9; i++)
		Wait200ns();
	JOYPAD_OUT_PORT &= ~(_BV(JOYPAD_LATCH_PIN));

	for(i=0; i<16; i++){
		joypad1_status_lo >>= 1; joypad2_status_lo >>= 1;
		JOYPAD_OUT_PORT &= ~(_BV(JOYPAD_CLOCK_PIN));
		if((JOYPAD_IN_PORT&(1<<JOYPAD_DATA1_PIN)) == 0)
			joypad1_status_lo |= (1<<15);
		if((JOYPAD_IN_PORT&(1<<JOYPAD_DATA2_PIN)) == 0)
			joypad2_status_lo |= (1<<15);
		JOYPAD_OUT_PORT |= _BV(JOYPAD_CLOCK_PIN);
		for(u8 j=0; j<34; j++)
			Wait200ns();
	}

	if(joypad1_status_lo == (BTN_START+BTN_SELECT+BTN_Y+BTN_B) || joypad2_status_lo == (BTN_START+BTN_SELECT+BTN_Y+BTN_B))
		SoftReset();

	for(i=0; i<9; i++)
		Wait200ns();

	for(i=0; i<16; i++){
		joypad1_status_hi <<= 1;
		joypad2_status_hi <<= 1;
		JOYPAD_OUT_PORT &= ~(_BV(JOYPAD_CLOCK_PIN));
		if((JOYPAD_IN_PORT&(1<<JOYPAD_DATA1_PIN)) == 0)
			joypad1_status_hi |= 1;
		if((JOYPAD_IN_PORT&(1<<JOYPAD_DATA2_PIN)) == 0)
			joypad2_status_hi |= 1;
		JOYPAD_OUT_PORT |= _BV(JOYPAD_CLOCK_PIN);
		for(u8 j=0; j<34; j++)
			Wait200ns();
	}
}


void UpdateCursor(u8 ylimit){
	InputDeviceHandler();
	oldpad = pad;
	pad = ReadJoypad(0);
	u8 speed = (pad & BTN_SR) ? 1 : 2;

	if(pad & BTN_LEFT){
		sprites[0].x = (sprites[0].x<speed) ? 0:(sprites[0].x-speed);
	}else if(pad & BTN_RIGHT){
		u16 m=(SCREEN_TILES_H*TILE_WIDTH)-9;
		sprites[0].x = (sprites[0].x+speed>m) ? m:(sprites[0].x+speed);
	}
	if(pad & BTN_UP){
		sprites[0].y = (sprites[0].y<speed) ? 0:(sprites[0].y-speed);
	}else if(pad & BTN_DOWN){
		u16 m=ylimit-8+3;
		sprites[0].y = (sprites[0].y+8+speed>m) ? (ylimit-8+3):(sprites[0].y+speed);
	}

	for(u8 i=0; i<2; i++){
		u16 p = ReadJoypad(i);
		if(!(p & MOUSE_SIGNATURE))
			continue;
		u8 xsign,ysign;
		u16 dx,dy;
		if(i==0){
			dx = (joypad1_status_hi & 0x007F);
			dy = (joypad1_status_hi>>8)&0x7F;
			xsign=(joypad1_status_hi>>15)&1;
			ysign=(joypad1_status_hi>>7)&1;
		}else{
			dx = (joypad2_status_hi & 0x007F);
			dy = (joypad2_status_hi>>8)&0x7F;
			xsign = (joypad2_status_hi>>15)&1;
			ysign = (joypad2_status_hi>>7)&1;
		}
		if(xsign){
			s16 nx=(s16)sprites[0].x - (s16)dx; sprites[0].x = (nx<0)?0:(u8)nx;
		}else{
			u16 nx=(u16)sprites[0].x + dx;
			u16 lim=(SCREEN_TILES_H*TILE_WIDTH)-8;
			sprites[0].x = (nx>lim) ? lim:(u8)nx;
		}
		if(ysign){
			s16 ny=(s16)sprites[0].y - (s16)dy;
			sprites[0].y = (ny<0)?0:(u8)ny;
		}else{
			u16 ny=(u16)sprites[0].y + dy;
			u16 lim=ylimit-4;
			sprites[0].y = (ny>lim) ? (u8)lim:(u8)ny;
		}
	}
}


static void PrintSongTitle(u8 x, u8 y, u8 len){
	for(u8 i=0; i<len; i++)
		SetTile(x+i,y,0);
}


void FileSelectWindow(){
	SilenceBuffer();
	ClearVram();
	SetRenderingParameters(33, SCREEN_TILES_V*TILE_HEIGHT);

	u8 layer = 1, loaded_dir = 0, notroot = 0;
	u16 foff = 0;
	u8 lastclick = 255, last_line = 255;

	while(1){
		WaitVsync(1);
		UpdateCursor(SCREEN_TILES_V*TILE_HEIGHT);
		u8 line = (u8)(sprites[0].y/8);
		u8 click=0;
		if(lastclick<20)
			lastclick++;
		if( (pad & (BTN_Y|BTN_SL|BTN_SR|BTN_MOUSE_LEFT)) && !(oldpad & (BTN_Y|BTN_SL|BTN_SR|BTN_MOUSE_LEFT)) )
			click=1;

		if(layer == 1){
			u8 *as8=(u8*)accum_span;
			DrawWindow(4,2,22,SCREEN_TILES_V-3,PSTR("Select File"),PSTR("Cancel"),NULL);
			u8 fline=0;
			if(line<4)
				fline = (foff)?foff+1:0;
			else if(line>3 && line<4+10 && (foff+(line-4))<total_files)
				fline = (u8)(foff+(line-3));
			else
				fline = (u8)(((foff+10)>total_files)?total_files:foff+10);
			PrintInt(16,SCREEN_TILES_V-1,fline,1);
			PrintInt(25,SCREEN_TILES_V-1,total_files,1);
			UMPrint(18,SCREEN_TILES_V-1,PSTR("of"));
			SetTile(26,2,TILE_WIN_SCRU);
			SetTile(26,SCREEN_TILES_V-1,TILE_WIN_SCRD);
			if(!loaded_dir){
				LoadDirData(0,0);
				loaded_dir=1;
				notroot = !IsRootDir();
				UMPrint(0,0,PSTR("Dir: "));
				SpiRamPrintString(5,0,(u32)(detected_ram-512),0,SCREEN_TILES_H-6);
			}
			UMPrint(5,3,notroot?PSTR(".."):PSTR("."));

			for(u16 i=0; i<10; i++){
				if(foff+i >= total_files)
					break;
				SpiRamPrintString(5,4+i,((foff+i)*32),0,0);
				if(line == (i+4)){
					for(u8 k=5;k<26;k++)
						vram[(line*VRAM_TILES_H)+k] += 64;
				}
			}

			if(last_line != line){
				last_line = line;
				if(line>3 && line<4+10 && (foff+(line-4))<total_files){
					u32 fbase = (u32)((foff+(line-4))*32);
					SpiRamReadInto((u8)(fbase>>16),(u16)(fbase&0xFFFF),as8,32);
					if(as8[0]!='/'){
						FRESULT res = pf_open((const char*)as8);
						if(!res){
							WORD br;
							pf_lseek(0);
							pf_read(as8,0x40,&br);
							while(rcv_spi()!=0xFF);
							UMPrint(0,1,PSTR("Title:"));
							UMPrintRam(7,1,(char*)as8);
						}else{
							UMPrint(0,1,PSTR("(File)"));
						}
					}else{
						UMPrint(0,1,PSTR("(Directory)"));
						for(u8 i=11; i<27; i++)
							SetTile(i,1,0);
					}
				}else{
					for(u8 i=0; i<27; i++)
						SetTile(i,1,0);
				}
			}

			if(click){
				if(ButtonHit(4, SCREEN_TILES_V-1, 6, 1))
					goto FILE_SELECT_END;
				else if(ButtonHit(26,2,1,1)){
					if(foff<10)
						foff = 0;
					else
						foff -= 10;
				}else if(ButtonHit(26,SCREEN_TILES_V-1,1,1)){
					if(foff+10 <= total_files)
						foff += 10;
				}else if(ButtonHit(5,4,20,10)){
					if(foff+(line-4) < total_files){
						SpiRamCopyStringNoBuffer(0, (u32)((foff+(line-4))*32), 32);
						char c = SpiRamReadU8(0,0);
						if(c=='/'){
							loaded_dir=0;
							SpiRamReadInto(0,1,as8,64);
							NextDir((char*)as8);
						}else{
							FRESULT fr = load_vgm(0);
							if(fr == FR_OK && start_vgm(1) == 0){
								g_ff_mul = 1; play_state = PS_LOADED | PS_PLAYING;
							}else{
								g_psg.paused = 1; SpiRamWriteU8(0,0,0); ClearVram(); loaded_dir=0;
							}
							goto FILE_SELECT_END;
						}
					}
				}else if(ButtonHit(5,3,20,1)){
					if(notroot){
						PreviousDir();
						loaded_dir=0;
					}
				}
			}
		}else{
			break;
		}
	}
FILE_SELECT_END:
	SpiRamCopyStringNoBuffer((u32)(detected_ram-64), 0, 64);
	play_state &= ~PS_DRAWN;
	PrintSongTitle((CONT_BAR_X/8),(CONT_BAR_Y/8)+2,20);
	if(sprites[0].y>20)
		sprites[0].y=18;
	WaitVsync(1);
}


void PlayerInterface(){
	UpdateCursor(24);

	u8 btn,newclick = 0;
	if(sprites[0].y >= (CONT_BAR_Y+CONT_BTN_H) || sprites[0].x < CONT_BAR_X || sprites[0].x >= CONT_BAR_X+CONT_BAR_W)
		btn=255;
	else
		btn = (u8)((sprites[0].x-CONT_BAR_X)/CONT_BTN_W);

	static u8 lastbtn=255;
	if( (pad & (BTN_Y|BTN_MOUSE_LEFT)) && !(oldpad & (BTN_Y|BTN_MOUSE_LEFT)) ){
		lastbtn = btn;
		newclick = 1;
	}

	if(lastbtn != 255){
		u8 moff = 2+(lastbtn*2);
		u8 xoff = (u8)((CONT_BAR_X/8)+(lastbtn*(CONT_BTN_W/8)));
		if((pad & BTN_Y)){
			SetTile(xoff+0,(CONT_BAR_Y/8)+0,pgm_read_byte(&pressed_map[moff++]));
			SetTile(xoff+1,(CONT_BAR_Y/8)+0,pgm_read_byte(&pressed_map[moff]));
			moff += (CONT_BAR_W/TILE_WIDTH)-1;
			SetTile(xoff+0,(CONT_BAR_Y/8)+1,pgm_read_byte(&pressed_map[moff++]));
			SetTile(xoff+1,(CONT_BAR_Y/8)+1,pgm_read_byte(&pressed_map[moff]));
		}else{
			SetTile(xoff+0,(CONT_BAR_Y/8)+0,pgm_read_byte(&buttons_map[moff++]));
			SetTile(xoff+1,(CONT_BAR_Y/8)+0,pgm_read_byte(&buttons_map[moff]));
			moff += (CONT_BAR_W/TILE_WIDTH)-1;
			SetTile(xoff+0,(CONT_BAR_Y/8)+1,pgm_read_byte(&buttons_map[moff++]));
			SetTile(xoff+1,(CONT_BAR_Y/8)+1,pgm_read_byte(&buttons_map[moff]));
			lastbtn = 255;
		}
	}

	g_ff_mul = 1;
	if(newclick){
		if(play_state & PS_LOADED){
			if(btn == 2){
				if(play_state & PS_PAUSE)
					play_state ^= PS_PAUSE;
				else
					play_state = PS_LOADED|PS_PAUSE;
				psg_vgm_pause( (play_state & PS_PAUSE) ? 1:0 );
			}else if(btn == 3){
				play_state = PS_LOADED|PS_DRAWN|PS_PLAYING;
				psg_vgm_pause(0);
			}else if(btn == 4){
				play_state = PS_LOADED|PS_DRAWN|PS_PLAYING;
			}
		}
		if(btn == 6){
			WaitVsync(1);
			FileSelectWindow();
			pad = oldpad = 0b0000111111111111;
			WaitVsync(1);
			play_state &= ~PS_DRAWN;
		}else if(btn == 8){
			if(masterVolume)
				masterVolume--;
		}else if(btn == 9){
			if(++masterVolume > 64)
				masterVolume = 64;
		}
	}
	if(lastbtn != 255){
		if(lastbtn == 4){
			g_ff_mul = 2;
		}
	}
}


void update_psg(){
	PlayerInterface();
	if(!(play_state & PS_DRAWN)){
		ClearVram();
		DrawMap(5,0,buttons_map);
		PrintSongTitle((CONT_BAR_X/8),(CONT_BAR_Y/8)+2,20);
		play_state |= PS_DRAWN;
	}

	if( (play_state & PS_PAUSE) || !(play_state & PS_PLAYING) ){
		SilenceBuffer();
		goto DRAW_TIMER;
	}

	if(++ptime_frame >= 60){
		ptime_frame=0;
		if(++ptime_sec > 59){
			ptime_sec=0;
			if(++ptime_min>99)
				ptime_min=99;
		}
	}

	u16 base = mix_bank ? 0u : 262u;

	psg_vgm_set_ff_mul(g_ff_mul);
	psg_vgm_mix_frame(&mix_buf[base]);

DRAW_TIMER:
	UMPrint(PTIME_X,PTIME_Y,PSTR("  :  :  "));
	PrintByte(PTIME_X+8,PTIME_Y,ptime_frame<<1,1);
	PrintByte(PTIME_X+5,PTIME_Y,ptime_sec,1);
	PrintByte(PTIME_X+2,PTIME_Y,ptime_min,0);
	UMPrintChar(PTIME_X+3,PTIME_Y,':');
	UMPrintChar(PTIME_X+6,PTIME_Y,':');
	SetRenderingParameters(33, 24);
}


int main(){
	SetFontTilesIndex(0);
	SetTileTable(tile_data);
	SetSpritesTileTable(tile_data);
	ClearVram();
	Intro();
	DrawWindow(1, 1, 28,10,NULL,NULL,NULL);
	WaitVsync(1);

	FRESULT res;
	u8 i;
	for(i=0; i<10 ; i++){
		res = pf_mount(&fs);
		if(!res){
			UMPrint(3,cony++,PSTR("Mounted SD Card"));
			i = 0;
			break;
		}
	}
	if(i){
		PrintByte(20,cony,res,0);
		UMPrint(3,cony++,PSTR("ERROR: SD Mount:"));
		goto MAIN_FAIL;
	}

	u8 bank_count = SpiRamInitGetSize();

	if(!bank_count){
		UMPrint(3,cony++,PSTR("ERROR: No SPI RAM detected"));
		goto MAIN_FAIL;
	}else{
		detected_ram = (u32)(bank_count*(64UL*1024UL));
		u8 moff = 21;
		if(bank_count > 1)
			moff++;
		if(bank_count > 15)
			moff++;

		PrintLong(moff,cony,(u32)detected_ram/1024UL);
		UMPrintChar(moff+1,cony,'K');
		UMPrint(3,cony++,PSTR("SPI RAM Detected:"));
		WaitVsync(60);
	}

	SpiRamWriteStringEntryFlash(0, PSTR("Select File ^"));
	NextDir(NULL);
	SpiRamPrintString(4,0,(u32)(detected_ram-512),0,4);
	SetRenderingParameters(33,16);

	sprites[0].tileIndex = TILE_CURSOR;
	sprites[0].flags = sprites[0].x = sprites[0].y = 0;

	while(1){
		WaitVsync(1);
		update_psg();
	}
MAIN_FAIL:
	WaitVsync(240);
	SoftReset();
	return 0;
}