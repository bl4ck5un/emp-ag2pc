#ifndef ABIT_H__
#define ABIT_H__

#include <emp-tool/emp-tool.h>
#include <emp-ot/emp-ot.h>
#include <immintrin.h>
#include "helper.h"
#include "c2pc_config.h"

namespace emp {

typedef __m256i dblock;
#define _mm256_set_m128i(v0, v1)  _mm256_insertf128_si256(_mm256_castsi128_si256(v1), (v0), 1)

class ABit { public:
#pragma GCC push_options
#pragma GCC optimize ("unroll-loops")
	inline block bit_matrix_mul(const __m256i& input) {
		uint8_t * tmp = (uint8_t *) (&input);
		block res = pre_table[tmp[0] + (0<<8)];
		for(int i = 1; i < l/8; ++i)
			res = xorBlocks(res, pre_table[tmp[i] + (i<<8)]);
		return res;
	}
#pragma GCC pop_options
	void pre_compute_table() {
		block tR[256];
		sse_trans((uint8_t *)(tR), (uint8_t*)R, 128, 256);
		for(int i = 0; i < 256/8; ++i)
			for (int j = 0; j < (1<<8); ++j) {
				pre_table[j + (i<<8)] = zero_block();
				for(int k = 0; k < 8; ++k) {
					if (((j >> k ) & 0x1) == 1)
						pre_table[j + (i<<8)] = xorBlocks(pre_table[j+ (i<<8)], tR[i*8+k]);
				}
			}
	}
	block *pre_table = nullptr;
	NetIO * io = nullptr;
	OTCO<NetIO> * base_ot = nullptr;
	PRG prg, *G0 = nullptr, *G1 = nullptr;
	PRP pi;
	bool setup = false;
	block *k0 = nullptr, *k1 = nullptr, *tmp = nullptr, *t = nullptr;
	bool *s = nullptr, *extended_r = nullptr;
	dblock *block_s, *tT = nullptr, * R = nullptr;
	block Delta;
	int l = 128, ssp, sspover8;
	const static int block_size = abit_block_size;
	ABit(NetIO * io, int ssp = 40) {
		this->io = io;
		this->ssp = ssp;
		this->base_ot = new OTCO<NetIO>(io);
		this->sspover8 = ssp/8;
		this->l +=ssp;
		this->pre_table = aalloc<block>((1<<8) * 256/8);
		this->s = new bool[l];
		this->k0 = aalloc<block>(l);
		this->k1 = aalloc<block>(l);
		this->G0 = new PRG[l];
		this->G1 = new PRG[l];
		this->R = (dblock*)aligned_alloc(32, 32*128);
		this->tT = (dblock*)aligned_alloc(32, 32*block_size);
		this->t = (block*)aligned_alloc(32, 32*block_size);
		this->block_s = (dblock*)aligned_alloc(32, 32);
		this->tmp = aalloc<block>(block_size/128);
		memset(t, 0, block_size * 32);

		PRG prg2(fix_key);
		prg2.random_data(R, 128*256/8);
		uint64_t hi = 0, lo = 0;
		if(ssp <=64)
			lo=((1ULL<<ssp)-1);

		dblock mask = _mm256_set_m128i(makeBlock(hi,lo), one_block());
		for(int i = 0; i < 128; ++i)
			R[i] = _mm256_and_si256(R[i], mask);
		pre_compute_table();
	}
	ABit * clone(NetIO * new_io, bool send) {
		ABit * new_abit = new ABit(new_io, ssp);
		if(send)
			new_abit->setup_send(k0, s);
		else
			new_abit->setup_recv(k0, k1);
		return new_abit;
	}

	block * out = nullptr;
	~ABit() {
		delete base_ot;
		delete_array_null(s);
		delete_array_null(G0);
		delete_array_null(G1);
		free(this->R);
		free(this->t);
		free(this->tT);
		free(k0);
		free(k1);
		free(tmp);
		free(pre_table);
		delete_array_null(extended_r);
		free(block_s);
	}

	void bool_to256(const bool * in, dblock * res) {
		bool tmpB[256];
		for(int i = 0; i < 256; ++i)tmpB[i] = false;
		memcpy(tmpB, in, l);
		uint64_t t1 = bool_to64(tmpB);
		uint64_t t2 = bool_to64(tmpB+64);
		uint64_t t3 = bool_to64(tmpB+128);
		uint64_t t4 = bool_to64(tmpB+192);
		*res = _mm256_set_epi64x(t4,t3,t2,t1);
	}

	void setup_send(block * in_k0 = nullptr, bool * in_s = nullptr) {
		setup = true;
		if(in_s != nullptr) {
			memcpy(k0, in_k0, l*sizeof(block));
			memcpy(s, in_s, l);
		} else {
			prg.random_bool(s, l);
			base_ot->recv(k0, s, l);
		}
		for(int i = 0; i < l; ++i)
			G0[i].reseed(&k0[i]);

		bool_to256(s, block_s);
		Delta = bit_matrix_mul(*block_s);
	}

	void setup_recv(block * in_k0 = nullptr, block * in_k1 =nullptr) {
		setup = true;
		if(in_k0 !=nullptr) {
			memcpy(k0, in_k0, l*sizeof(block));
			memcpy(k1, in_k1, l*sizeof(block));
		} else {
			prg.random_block(k0, l);
			prg.random_block(k1, l);
			base_ot->send(k0, k1, l);
		}
		for(int i = 0; i < l; ++i) {
			G0[i].reseed(&k0[i]);
			G1[i].reseed(&k1[i]);
		}

	}
	int padded_length(int length){
		return ((length+128+ssp+block_size-1)/block_size)*block_size;
	}
	void send_pre(int length) {
		length = padded_length(length);
		if(!setup) setup_send();

		for (int j = 0; j < length/block_size; ++j) {
			for(int i = 0; i < l; ++i) {
				io->recv_data(tmp, block_size/8);
				G0[i].random_data(t+(i*block_size/128), block_size/8);
				if (s[i])
					xorBlocks_arr(t+(i*block_size/128), t+(i*block_size/128), tmp, block_size/128);
			}
			sse_trans((uint8_t *)(tT), (uint8_t*)t, 256, block_size);
			for(int i = 0; i < block_size; ++i)
				out[j*block_size + i] = bit_matrix_mul(tT[i]);
		}
	}

	void recv_pre(const bool* r, int length) {
		int old_length = length;
		length = padded_length(length);

		if(!setup)setup_recv();

		bool * r2 = new bool[length];
		memcpy(r2, r, old_length);
		delete_array_null(extended_r);
		extended_r = new bool[length - old_length];
		prg.random_bool(extended_r, length- old_length);
		memcpy(r2+old_length, extended_r, length - old_length);

		block *block_r = aalloc<block>(length/128);
		for(int i = 0; i < length/128; ++i) {
			block_r[i] = bool_to128(r2+i*128);
		}

		for (int j = 0; j * block_size < length; ++j) {
			for(int i = 0; i < l; ++i) {
				G0[i].random_data(t+(i*block_size/128), block_size/8);
				G1[i].random_data(tmp, block_size/8);
				xorBlocks_arr(tmp, t+(i*block_size/128), tmp, block_size/128);
				xorBlocks_arr(tmp, block_r+(j*block_size/128), tmp, block_size/128);
				io->send_data(tmp, block_size/8);
			}
			sse_trans((uint8_t *)tT, (uint8_t*)t, 256, block_size);

			for(int i = 0; i < block_size; ++i)
				out[j*block_size + i] = bit_matrix_mul(tT[i]);
		}
		free(block_r);
		delete[] r2;
	}

	void send(block * data, int length) {
		out = aalloc<block>(padded_length(length));
		send_pre(length);
		if(!send_check(length))
			error("OT Extension check failed");
		memcpy(data, out, sizeof(block)*length);
		free(out);
	}
	void recv(block* data, const bool* b, int length) {
		out = aalloc<block>(padded_length(length));
		recv_pre(b, length);
		recv_check(b, length);
		memcpy(data, out, sizeof(block)*length);
		free(out);
	}

	bool send_check(int length) {
		int extended_length = padded_length(length);
		block seed2, x, t[2], q[2], tmp1, tmp2;
		io->recv_block(&seed2, 1);
		block *chi = aalloc<block>(extended_length);
		PRG prg2(&seed2);
		prg2.random_block(chi, extended_length);

		q[0] = zero_block();
		q[1] = zero_block();
		for(int i = 0; i < extended_length; ++i) {
			mul128(out[i], chi[i], &tmp1, &tmp2);
			q[0] = xorBlocks(q[0], tmp1);
			q[1] = xorBlocks(q[1], tmp2);
		}
		io->recv_block(&x, 1);
		io->recv_block(t, 2);

		mul128(x, Delta, &tmp1, &tmp2);
		q[0] = xorBlocks(q[0], tmp1);
		q[1] = xorBlocks(q[1], tmp2);

		free(chi);
		return block_cmp(q, t, 2);	
	}
	void recv_check(const bool* r, int length) {
		int extended_length = padded_length(length);
		block *chi = aalloc<block>(extended_length);
		block seed2, x = zero_block(), t[2], tmp1, tmp2;
		prg.random_block(&seed2,1);
		io->send_block(&seed2, 1);
		PRG prg2(&seed2);
		t[0] = t[1] = zero_block();
		prg2.random_block(chi,extended_length);
		for(int i = 0 ; i < length; ++i) {
			if(r[i])
				x = xorBlocks(x, chi[i]);
		}
		for(int i = 0 ; i < extended_length - length; ++i) {
			if(extended_r[i])
				x = xorBlocks(x, chi[i+length]);
		}

		io->send_block(&x, 1);
		for(int i = 0 ; i < extended_length; ++i) {
			mul128(chi[i], out[i], &tmp1, &tmp2);
			t[0] = xorBlocks(t[0], tmp1);
			t[1] = xorBlocks(t[1], tmp2);
		}
		io->send_block(t, 2);

		free(chi);
	}
};
}
#endif// ABIT_H__