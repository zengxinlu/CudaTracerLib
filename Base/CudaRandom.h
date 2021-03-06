#pragma once

#include <Math/Vector.h>
#include "curand_kernel.h"

namespace CudaTracerLib {

class LinearCongruental_GENERATOR
{
	unsigned int X_i;
public:
	CUDA_FUNC_IN explicit LinearCongruental_GENERATOR(unsigned int seed = 12345)
	{
		X_i = seed;
	}

	CUDA_FUNC_IN unsigned int randomUint()
	{
		unsigned int a = 1664525, c = 1013904223;
		X_i = (a * X_i + c);
		return X_i;
	}

	CUDA_FUNC_IN float randomFloat()
	{
		return float(randomUint()) / float(UINT_MAX);
	}
};

class Lehmer_GENERATOR
{
	unsigned int X_i;
public:
	CUDA_FUNC_IN explicit Lehmer_GENERATOR(unsigned int seed = 12345)
	{
		X_i = seed;
	}

	CUDA_FUNC_IN unsigned int randomUint()
	{
		const unsigned int a = 16807;
		const unsigned int m = UINT_MAX;
		X_i = (X_i * a) % m;
		return X_i;
	}

	CUDA_FUNC_IN float randomFloat()
	{
		return randomUint() / float(UINT_MAX);
	}
};

class TAUSWORTHE_GENERATOR
{
	CUDA_FUNC_IN static unsigned int TausStep(unsigned int &z, unsigned int S1, unsigned int S2, unsigned int S3, unsigned int M)
	{
		unsigned int b = (((z << S1) ^ z) >> S2);
		return z = (((z & M) << S3) ^ b);
	}
	CUDA_FUNC_IN static unsigned int LCGStep(unsigned int &z, unsigned int A, unsigned int C)
	{
		return z = (A*z + C);
	}
	unsigned int z1, z2, z3, z4;
public:
	CUDA_FUNC_IN explicit TAUSWORTHE_GENERATOR(unsigned int seed = 12345)
	{
		z1 = seed + 1 + 1;
		z2 = seed + 7 + 1;
		z3 = seed + 15 + 1;
		z4 = seed + 127 + 1;
	}

	CUDA_FUNC_IN unsigned int randomUint()
	{
		return TausStep(z1, 13, 19, 12, 4294967294UL) ^ TausStep(z2, 2, 25, 4, 4294967288UL) ^ TausStep(z3, 3, 11, 17, 4294967280UL) ^ LCGStep(z4, 1664525, 1013904223UL);
	}

	CUDA_FUNC_IN float randomFloat()
	{
		return float(randomUint()) / float(UINT_MAX);
	}
};

class Xorshift_GENERATOR
{
	unsigned int y;
public:
	CUDA_FUNC_IN explicit Xorshift_GENERATOR(unsigned int seed = 12345)
	{
		y = seed;
	}

	CUDA_FUNC_IN unsigned int randomUint()
	{
		y = y ^ (y << 13);
		y = y ^ (y >> 17);
		y = y ^ (y << 5);
		return y;
	}

	CUDA_FUNC_IN float randomFloat()
	{
		return float(randomUint()) / float(UINT_MAX);
	}
};

struct Curand_GENERATOR
{
	curandState state;
private:
	unsigned int curand2(curandStateXORWOW_t *state)
	{
		unsigned int t;
		t = (state->v[0] ^ (state->v[0] >> 2));
		state->v[0] = state->v[1];
		state->v[1] = state->v[2];
		state->v[2] = state->v[3];
		state->v[3] = state->v[4];
		state->v[4] = (state->v[4] ^ (state->v[4] << 4)) ^ (t ^ (t << 1));
		state->d += 362437;
		return state->v[4] + state->d;
	}
	float curand_uniform2(unsigned int x)
	{
		return x * CURAND_2POW32_INV + (CURAND_2POW32_INV / 2.0f);
	}
	void __curand_matvec(unsigned int *vector, unsigned int *matrix, unsigned int *result, int n)
	{
		for (int i = 0; i < n; i++) {
			result[i] = 0;
		}
		for (int i = 0; i < n; i++) {
			for (int j = 0; j < 32; j++) {
				if (vector[i] & (1 << j)) {
					for (int k = 0; k < n; k++) {
						result[k] ^= matrix[n * (i * 32 + j) + k];
					}
				}
			}
		}
	}
	void __curand_veccopy(unsigned int *vector, unsigned int *vectorA, int n)
	{
		for (int i = 0; i < n; i++) {
			vector[i] = vectorA[i];
		}
	}
	void __curand_matcopy(unsigned int *matrix, unsigned int *matrixA, int n)
	{
		for (int i = 0; i < n * n * 32; i++) {
			matrix[i] = matrixA[i];
		}
	}
	void __curand_matmat(unsigned int *matrixA, unsigned int *matrixB, int n)
	{
		unsigned int result[MAX_XOR_N];
		for (int i = 0; i < n * 32; i++) {
			__curand_matvec(matrixA + i * n, matrixB, result, n);
			for (int j = 0; j < n; j++) {
				matrixA[i * n + j] = result[j];
			}
		}
	}
	template <typename T, int n> void _skipahead_sequence_scratch(unsigned long long x, T *state, unsigned int *scratch)
	{
		// unsigned int matrix[n * n * 32];
		unsigned int *matrix = scratch;
		// unsigned int matrixA[n * n * 32];
		unsigned int *matrixA = scratch + (n * n * 32);
		// unsigned int vector[n];
		unsigned int *vector = scratch + (n * n * 32) + (n * n * 32);
		// unsigned int result[n];
		unsigned int *result = scratch + (n * n * 32) + (n * n * 32) + n;
		unsigned long long p = x;
		for (int i = 0; i < n; i++) {
			vector[i] = state->v[i];
		}
		int matrix_num = 0;
		while (p && matrix_num < PRECALC_NUM_MATRICES - 1) {
			for (unsigned int t = 0; t < (p & PRECALC_BLOCK_MASK); t++) {
				__curand_matvec(vector, precalc_xorwow_matrix_host[matrix_num], result, n);
				__curand_veccopy(vector, result, n);
			}
			p >>= PRECALC_BLOCK_SIZE;
			matrix_num++;
		}
		if (p) {
			__curand_matcopy(matrix, precalc_xorwow_matrix_host[PRECALC_NUM_MATRICES - 1], n);
			__curand_matcopy(matrixA, precalc_xorwow_matrix_host[PRECALC_NUM_MATRICES - 1], n);
		}
		while (p) {
			for (unsigned int t = 0; t < (p & SKIPAHEAD_MASK); t++) {
				__curand_matvec(vector, matrixA, result, n);
				__curand_veccopy(vector, result, n);
			}
			p >>= SKIPAHEAD_BLOCKSIZE;
			if (p) {
				for (int i = 0; i < SKIPAHEAD_BLOCKSIZE; i++) {
					__curand_matmat(matrix, matrixA, n);
					__curand_matcopy(matrixA, matrix, n);
				}
			}
		}
		for (int i = 0; i < n; i++) {
			state->v[i] = vector[i];
		}
	}
	template <typename T, int n> void _skipahead_scratch(unsigned long long x, T *state, unsigned int *scratch)
	{
		// unsigned int matrix[n * n * 32];
		unsigned int *matrix = scratch;
		// unsigned int matrixA[n * n * 32];
		unsigned int *matrixA = scratch + (n * n * 32);
		// unsigned int vector[n];
		unsigned int *vector = scratch + (n * n * 32) + (n * n * 32);
		// unsigned int result[n];
		unsigned int *result = scratch + (n * n * 32) + (n * n * 32) + n;
		unsigned long long p = x;
		for (int i = 0; i < n; i++) {
			vector[i] = state->v[i];
		}
		int matrix_num = 0;
		while (p && (matrix_num < PRECALC_NUM_MATRICES - 1)) {
			for (unsigned int t = 0; t < (p & PRECALC_BLOCK_MASK); t++) {
				__curand_matvec(vector, precalc_xorwow_offset_matrix_host[matrix_num], result, n);
				__curand_veccopy(vector, result, n);
			}
			p >>= PRECALC_BLOCK_SIZE;
			matrix_num++;
		}
		if (p) {
			__curand_matcopy(matrix, precalc_xorwow_offset_matrix_host[PRECALC_NUM_MATRICES - 1], n);
			__curand_matcopy(matrixA, precalc_xorwow_offset_matrix_host[PRECALC_NUM_MATRICES - 1], n);
		}
		while (p) {
			for (unsigned int t = 0; t < (p & SKIPAHEAD_MASK); t++) {
				__curand_matvec(vector, matrixA, result, n);
				__curand_veccopy(vector, result, n);
			}
			p >>= SKIPAHEAD_BLOCKSIZE;
			if (p) {
				for (int i = 0; i < SKIPAHEAD_BLOCKSIZE; i++) {
					__curand_matmat(matrix, matrixA, n);
					__curand_matcopy(matrixA, matrix, n);
				}
			}
		}
		for (int i = 0; i < n; i++) {
			state->v[i] = vector[i];
		}
		state->d += 362437 * (unsigned int)x;
	}
	void _curand_init_scratch(unsigned long long seed,
		unsigned long long subsequence,
		unsigned long long offset,
		curandStateXORWOW_t *state,
		unsigned int *scratch)
	{
		unsigned int s0 = ((unsigned int)seed) ^ 0xaad26b49UL;
		unsigned int s1 = (unsigned int)(seed >> 32) ^ 0xf7dcefddUL;
		unsigned int t0 = 1099087573UL * s0;
		unsigned int t1 = 2591861531UL * s1;
		state->d = 6615241 + t1 + t0;
		state->v[0] = 123456789UL + t0;
		state->v[1] = 362436069UL ^ t0;
		state->v[2] = 521288629UL + t1;
		state->v[3] = 88675123UL ^ t1;
		state->v[4] = 5783321UL + t0;
		_skipahead_sequence_scratch<curandStateXORWOW_t, 5>(subsequence, state, scratch);
		_skipahead_scratch<curandStateXORWOW_t, 5>(offset, state, scratch);
		state->boxmuller_flag = 0;
		state->boxmuller_flag_double = 0;
	}
	void curand_init2(unsigned long long seed,
		unsigned long long subsequence,
		unsigned long long offset,
		curandStateXORWOW_t *state)
	{
		unsigned int scratch[5 * 5 * 32 * 2 + 5 * 2];
		_curand_init_scratch(seed, subsequence, offset, state, (unsigned int*)scratch);
	}
public:
	CUDA_FUNC_IN explicit Curand_GENERATOR(unsigned int a_Index = 12345)
	{
		Initialize(a_Index);
	}
	CTL_EXPORT CUDA_DEVICE CUDA_HOST void Initialize(unsigned int a_Index);
	CTL_EXPORT CUDA_DEVICE CUDA_HOST float randomFloat();
	CTL_EXPORT CUDA_DEVICE CUDA_HOST unsigned long randomUint();
};

struct CudaRNG : public Curand_GENERATOR
{
	CudaRNG() = default;
	CUDA_FUNC_IN explicit CudaRNG(unsigned int seed)
		: Curand_GENERATOR(seed)
	{

	}
	CUDA_FUNC_IN void Initialize(unsigned int a_Index)
	{
		*this = CudaRNG(a_Index);
	}
	CUDA_FUNC_IN Vec2f randomFloat2()
	{
		return Vec2f(randomFloat(), randomFloat());
	}
	CUDA_FUNC_IN Vec3f randomFloat3()
	{
		return Vec3f(randomFloat(), randomFloat(), randomFloat());
	}
	CUDA_FUNC_IN Vec4f randomFloat4()
	{
		return Vec4f(randomFloat(), randomFloat(), randomFloat(), randomFloat());
	}
};

}