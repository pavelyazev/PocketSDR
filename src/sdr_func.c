// 
//  Pocket SDR C Library - Fundamental GNSS SDR Functions
//
//  Author:
//  T.TAKASU
//
//  History:
//  2022-01-25  1.0  new
//  2022-05-11  1.1  add API: search_code(), corr_max(), fine_dop(),
//                   shift_freq(), dop_bins(), add_buff(), xor_bits()
//  2022-05-18  1.2  change API: *() -> sdr_*()
//  2022-05-23  1.3  add API: sdr_read_data(), sdr_parse_nums()
//  2022-07-08  1.4  port sdr_func.py to C
//  2022-08-04  1.5  ftell(),fseek() -> fgetpos(),fsetpos()
//  2022-08-15  1.6  ensure thread-safety of sdr_corr_fft_()
//  2023-12-28  1.7  support API changes
//                   enable escape sequence for Windows console
//  2024-01-03  1.8  fix AVX2 codes in dot_cpx_real()
//  2024-01-18  1.9  add API sdr_cpx_mul()
//  2024-03-20  1.10 add API sdr_buff_new(), sdr_buff_free()
//                   modify API sdr_read_data(), sdr_search_code(),
//                   sdr_mix_carr(), sdr_corr_std(), sdr_corr_fft()
//
#include <math.h>
#include <stdarg.h>
#include "rtklib.h"
#include "pocket_sdr.h"

#ifdef WIN32
#include <io.h>
#endif
#ifdef AVX2
#include <immintrin.h>
#endif

// constants and macros --------------------------------------------------------
#define NTBL          256   // carrier-mixed-data LUT size
#define CSCALE        10.0f // carrier scale (max(IQ) * sqrt(2) * scale < 127)
#define DOP_STEP      0.5   // Doppler frequency search step (* 1 / code cycle)
#define MAX_FFTW_PLAN 32    // max number of FFTW plans
#define FFTW_FLAG     FFTW_ESTIMATE // FFTW flag

#define SQR(x)        ((x) * (x))
#define MIN(x, y)     ((x) < (y) ? (x) : (y))
#define ROUND(x)      floor(x + 0.5)

// global variables ------------------------------------------------------------
static sdr_cpx16_t mix_tbl[NTBL*256] = {{0}}; // carrier-mixed-data LUT
static fftwf_plan fftw_plans[MAX_FFTW_PLAN][2] = {{0}}; // FFTW plan buffer
static int fftw_size[MAX_FFTW_PLAN] = {0}; // FFTW plan sizes
static int log_lvl = 3;           // log level
static stream_t log_str = {0};    // log stream

// enable escape sequence for Windows console ----------------------------------
static void enable_console_esc(void)
{
#ifdef WIN32
    HANDLE h = (HANDLE)_get_osfhandle(1); // stdout
    DWORD mode = 0;
    
    if (!GetConsoleMode(h, &mode) ||
        !SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        //fprintf(stderr, "SetConsoleMode() error (%ld)\n", GetLastError());
    }
#endif
}

// initialize GNSS SDR functions -----------------------------------------------
void sdr_func_init(const char *file)
{
    // initilize log stream
    strinitcom();
    strinit(&log_str);
    
    // import FFTW wisdom 
    if (*file && !fftwf_import_wisdom_from_filename(file)) {
        fprintf(stderr, "FFTW wisdom import error %s\n", file);
    }
    // generate carrier-mixed-data LUT
    for (int i = 0; i < NTBL; i++) {
        int8_t carr_I = (int8_t)ROUND(cos(-2.0 * PI * i / NTBL) * CSCALE);
        int8_t carr_Q = (int8_t)ROUND(sin(-2.0 * PI * i / NTBL) * CSCALE);
        for (int j = 0; j < 256; j++) {
            int8_t I = SDR_CPX8_I(j), Q = SDR_CPX8_Q(j);
            mix_tbl[(j << 8) + i].I = I * carr_I - Q * carr_Q;
            mix_tbl[(j << 8) + i].Q = I * carr_Q + Q * carr_I;
        }
    }
    // enable escape sequence for Windows console
    enable_console_esc();
}

//------------------------------------------------------------------------------
//  Allocate memory for complex array. If no memory allocated, it exits the AP
//  immediately with an error message.
//  
//  args:
//      N        (I)  Size of complex array
//
//  return:
//      Complex array allocated.
//
sdr_cpx_t *sdr_cpx_malloc(int N)
{
    sdr_cpx_t *cpx;
    
    if (!(cpx = (sdr_cpx_t *)fftwf_malloc(sizeof(sdr_cpx_t) * N))) {
        fprintf(stderr, "sdr_cpx_t memory allocation error N=%d\n", N);
        exit(-1);
    }
    return cpx;
}

//------------------------------------------------------------------------------
//  Free memory allocated by sdr_cpx_malloc().
//  
//  args:
//      cpx      (I)  Complex array
//
//  return:
//      None
//
void sdr_cpx_free(sdr_cpx_t *cpx)
{
    fftwf_free(cpx);
}

//------------------------------------------------------------------------------
//  Absolute value of a complex.
//  
//  args:
//      cpx      (I)  Complex value
//
//  return:
//      | cpx |
//
float sdr_cpx_abs(sdr_cpx_t cpx)
{
    return sqrtf(SQR(cpx[0]) + SQR(cpx[1]));
}

//------------------------------------------------------------------------------
//  Multiplication of two complex arrays.
//  
//  args:
//      a, b     (I)  Complex arrays
//      N        (I)  Size of comlex arrays
//      s        (I)  Scale
//      c        (O)  Multiplication of a and b (c[i] = a[i] * b[i] * s)
//
//  return:
//      None
//
void sdr_cpx_mul(const sdr_cpx_t *a, const sdr_cpx_t *b, int N, float s,
    sdr_cpx_t *c)
{
    for (int i = 0; i < N; i++) {
        c[i][0] = (a[i][0] * b[i][0] - a[i][1] * b[i][1]) * s;
        c[i][1] = (a[i][0] * b[i][1] + a[i][1] * b[i][0]) * s;
    }
}

//------------------------------------------------------------------------------
//  Generate a new IF data buffer.
//
//  args:
//      N        (I)  Size of IF data buffer
//      IQ       (I)  Sampling type (1: I-sampling, 2: IQ-sampling)
//
//  return:
//      IF data buffer
//
sdr_buff_t *sdr_buff_new(int N, int IQ)
{
    sdr_buff_t *buff = (sdr_buff_t *)sdr_malloc(sizeof(sdr_buff_t));
    buff->data = (sdr_cpx8_t *)sdr_malloc(sizeof(sdr_cpx8_t *) * N);
    buff->N = N;
    buff->IQ = IQ;
    return buff;
}

//------------------------------------------------------------------------------
//  Free IF data buffer.
//
//  args:
//      buff     (I)  IF data buffer
//
//  return:
//      none
//
void sdr_buff_free(sdr_buff_t *buff)
{
    if (!buff) return;
    sdr_free(buff->data);
    sdr_free(buff);
}

//------------------------------------------------------------------------------
//  Read digitalized IF (inter-frequency) data from file. Supported file format
//  is signed byte (int8) for I-sampling (real-sampling) or interleaved singned
//  byte for IQ-sampling (complex-sampling).
//
//  args:
//      file     (I) Digitalized IF data file path
//      fs       (I) Sampling frequency (Hz)
//      IQ       (I) Sampling type (1: I-sampling, 2: IQ-sampling)
//      T        (I) Sample period (s) (0: all samples)
//      toff     (I) Time offset from the beginning (s)
//
//  return:
//      IF data buffer (NULL: read error)
//
sdr_buff_t *sdr_read_data(const char *file, double fs, int IQ, double T,
    double toff)
{
    size_t cnt = (T > 0.0) ? (size_t)(fs * T * IQ) : 0;
    size_t off = (size_t)(fs * toff * IQ);
    FILE *fp;
    
    if (!(fp = fopen(file, "rb"))) {
        fprintf(stderr, "data read error %s\n", file);
        return NULL;
    }
    // get file size
    fseek(fp, 0, SEEK_END);
#ifdef WIN32
    fpos_t pos = 0;
    fgetpos(fp, &pos);
    size_t size = (size_t)pos;
#else
    fpos_t pos = {0};
    fgetpos(fp, &pos);
    size_t size = (size_t)(pos.__pos);
#endif
    rewind(fp);
    
    if (cnt <= 0) {
        cnt = size - off;
    }
    if (size < off + cnt) {
        fclose(fp);
        return NULL;
    }
    int8_t *raw = (int8_t *)sdr_malloc(cnt);
#ifdef WIN32
    pos = (size_t)off;
    fsetpos(fp, &pos);
#else
    pos.__pos = (__off_t)off;
    fsetpos(fp, &pos);
#endif
    
    if (fread(raw, 1, cnt, fp) < cnt) {
        fprintf(stderr, "data read error %s\n", file);
        fclose(fp);
        return NULL;
    }
    sdr_buff_t *buff = sdr_buff_new(cnt / IQ, IQ);
    
    if (IQ == 1) { // I-sampling
        for (int i = 0; i < buff->N; i++) {
            buff->data[i] = SDR_CPX8(raw[i], 0);
        }
    }
    else { // IQ-sampling
        for (int i = 0; i < buff->N; i++) {
            buff->data[i] = SDR_CPX8(raw[i*2], raw[i*2+1]);
        }
    }
    sdr_free(raw);
    fclose(fp);
    return buff;
}

//------------------------------------------------------------------------------
//  Parallel code search in digitized IF data.
//
//  args:
//      code_fft (I) Code DFT (with or w/o zero-padding) as complex array
//      T        (I) Code cycle (period) (s)
//      buff     (I) IF data buffer
//      ix       (I) Index of sample data
//      N        (I) length of sample data
//      fs       (I) Sampling frequency (Hz)
//      fi       (I) IF frequency (Hz)
//      fds      (I) Doppler frequency bins as ndarray (Hz)
//      len_fds  (I) length of Doppler frequency bins
//      P        (IO) Correlation powers in the Doppler frequencies - Code offset
//                   space as float 2D-array (N x len_fs, N = (int)(fs * T))
//
//  return:
//      none
//
void sdr_search_code(const sdr_cpx_t *code_fft, double T,
    const sdr_buff_t *buff, int ix, int N, double fs, double fi,
    const float *fds, int len_fds, float *P)
{
    sdr_cpx_t *C = sdr_cpx_malloc(N);
    
    for (int i = 0; i < len_fds; i++) {
        
        // FFT correlator
        sdr_corr_fft(buff, ix, N, fs, fi + fds[i], 0.0, code_fft, C);
        
        // add correlation power
        for (int j = 0; j < N; j++) {
            P[i*N+j] += SQR(C[j][0]) + SQR(C[j][1]); // abs(C[j]) ** 2
        }
        if (i % 22 == 21) { // release cpu
            sdr_sleep_msec(1);
        }
    }
    sdr_cpx_free(C);
}

// max correlation power and C/N0 ----------------------------------------------
float sdr_corr_max(const float *P, int N, int Nmax, int M, double T, int *ix)
{
    float P_max = 0.0, P_ave = 0.0;
    int n = 0;
    
    for (int i = 0; i < M; i++) {
        for (int j = 0; j < Nmax; j++) {
            P_ave += (P[i*N+j] - P_ave) / ++n;
            if (P[i*N+j] <= P_max) continue;
            P_max = P[i*N+j];
            ix[0] = i; // index of doppler freq.
            ix[1] = j; // index of code offset
        }
    }
    return (P_ave > 0.0f) ? 10.0f * log10f((P_max - P_ave) / P_ave / T) : 0.0f;
}

// polynomial fitting ----------------------------------------------------------
static int poly_fit(const double *x, const double *y, int nx, int np, double *p)
{
    if (nx < np) {
        return 0;
    }
    double *V = mat(np, nx), *Q = mat(np, np);
    
    for (int i = 0; i < nx; i++) { // Vandermonde matrix
        for (int j = 0; j < np; j++) {
            V[i*np+j] = (j == 0) ? 1.0 : V[i*np+j-1] * x[i];
        }
    }
    int stat = lsq(V, y, np, nx, p, Q);
    free(V);
    free(Q);
    return !stat;
}

// fine Doppler frequency by quadratic fitting ---------------------------------
double sdr_fine_dop(const float *P, int N, const float *fds, int len_fds,
    const int *ix)
{
    if (ix[0] == 0 || ix[0] == len_fds - 1) {
        return fds[ix[0]];
    }
    double x[3], y[3], p[3];
    
    for (int i = 0; i < 3; i++) {
        x[i] = fds[ix[0]-1+i];
        y[i] = P[(ix[0]-1+i)*N+ix[1]];
    }
    if (!poly_fit(x, y, 3, 3, p)) {
        return fds[ix[0]];
    }
    return -p[1] / (2.0 * p[2]);
}

// shift IF frequency for GLONASS FDMA -----------------------------------------
double sdr_shift_freq(const char *sig, int fcn, double fi)
{
    if (!strcmp(sig, "G1CA")) {
        fi += 0.5625e6 * fcn;
    }
    else if (!strcmp(sig, "G2CA")) {
        fi += 0.4375e6 * fcn;
    }
    return fi;
}

// doppler search bins ---------------------------------------------------------
float *sdr_dop_bins(double T, float dop, float max_dop, int *len_fds)
{
    float *fds, step = DOP_STEP / T;
    
    *len_fds = (int)(2.0 * max_dop / step) + 1;
    fds = (float *)sdr_malloc(sizeof(float) * (*len_fds));
    
    for (int i = 0; i < *len_fds; i++) {
        fds[i] = dop - max_dop + i * step;
    }
    return fds;
}

// mix carrier -----------------------------------------------------------------
static void mix_carr(const sdr_buff_t *buff, int ix, int N, double phi,
    double step, sdr_cpx16_t *IQ)
{
    const uint8_t *data = buff->data + ix;
    double scale = 1 << 24;
    uint32_t p = (uint32_t)(phi * scale), s = (uint32_t)(step * scale);
    int i = 0;
#ifdef AVX2
    __m256i yphi = _mm256_set_epi32(p+s*7, p+s*6, p+s*5, p+s*4, p+s*3, p+s*2, p+s, p);
    __m256i ystp = _mm256_set_epi32(s*8, s*8, s*8, s*8, s*8, s*8, s*8, s*8);
    
    for ( ; i < N - 16; i += 8) {
        int idx[8];
        __m128i xdas = _mm_loadu_si128((__m128i *)(data + i));
        __m256i ydat = _mm256_cvtepu8_epi32(xdas);
        __m256i yidx = _mm256_add_epi32(_mm256_slli_epi32(ydat, 8),
            _mm256_srli_epi32(yphi, 24));
        _mm256_storeu_si256((__m256i *)idx, yidx);
        IQ[i  ] = mix_tbl[idx[0]];
        IQ[i+1] = mix_tbl[idx[1]];
        IQ[i+2] = mix_tbl[idx[2]];
        IQ[i+3] = mix_tbl[idx[3]];
        IQ[i+4] = mix_tbl[idx[4]];
        IQ[i+5] = mix_tbl[idx[5]];
        IQ[i+6] = mix_tbl[idx[6]];
        IQ[i+7] = mix_tbl[idx[7]];
        yphi = _mm256_add_epi32(yphi, ystp);
    }
#endif
    for (p += s * i; i < N; i++, p += s) {
        int idx = ((int)data[i] << 8) + (p >> 24);
        IQ[i] = mix_tbl[idx];
    }
}

void sdr_mix_carr(const sdr_buff_t *buff, int ix, int N, double fs, double fc,
    double phi, sdr_cpx16_t *IQ)
{
    double step = fc / fs * NTBL;
    phi = fmod(phi, 1.0) * NTBL;
    
    if (ix + N <= buff->N) {
        mix_carr(buff, ix, N, phi, step, IQ);
    }
    else { // across IF buffer boundary
        int n = buff->N - ix;
        mix_carr(buff, ix, n, phi, step, IQ);
        mix_carr(buff, 0, N - n, phi + step * n, step, IQ + n);
    }
}

// sum 16 bit x 16 fields ------------------------------------------------------
#define sum_16x16(ymm, sum) { \
    int16_t s[16]; \
    _mm256_storeu_si256((__m256i *)s, ymm); \
    ymm = _mm256_setzero_si256(); \
    sum += s[0] + s[1] + s[2] + s[3] + s[4] + s[5] + s[6] + s[7] + s[8] + s[9] \
        + s[10] + s[11] + s[12] + s[13] + s[14] + s[15]; \
}

// inner product of IQ data and code -------------------------------------------
static void dot_IQ_code(const sdr_cpx16_t *IQ, const sdr_cpx16_t *code, int N,
    float s, sdr_cpx_t *c)
{
    int i = 0;
    (*c)[0] = (*c)[1] = 0.0f;
#ifdef AVX2
    __m256i ysumI = _mm256_setzero_si256();
    __m256i ysumQ = _mm256_setzero_si256();
    __m256i yextI = _mm256_set_epi8(0,1,0,1,0,1,0,1, 0,1,0,1,0,1,0,1,
        0,1,0,1,0,1,0,1, 0,1,0,1,0,1,0,1);
    __m256i yextQ = _mm256_set_epi8(1,0,1,0,1,0,1,0, 1,0,1,0,1,0,1,0,
        1,0,1,0,1,0,1,0, 1,0,1,0,1,0,1,0);
    
    for ( ; i < N - 15; i += 16) {
        __m256i ydata = _mm256_loadu_si256((__m256i *)(IQ + i));
        __m256i ycode = _mm256_loadu_si256((__m256i *)(code + i)); // {-1,0,1}
        __m256i ycorr = _mm256_sign_epi8(ydata, ycode); // IQ * code
        ysumI = _mm256_add_epi16(ysumI, _mm256_maddubs_epi16(yextI, ycorr));
        ysumQ = _mm256_add_epi16(ysumQ, _mm256_maddubs_epi16(yextQ, ycorr));
        if (i % (16 * 256) == 0) {
            sum_16x16(ysumI, (*c)[0])
            sum_16x16(ysumQ, (*c)[1])
        }
    }
    sum_16x16(ysumI, (*c)[0])
    sum_16x16(ysumQ, (*c)[1])
#endif
    for ( ; i < N; i++) {
        (*c)[0] += IQ[i].I * code[i].I;
        (*c)[1] += IQ[i].Q * code[i].Q;
    }
    (*c)[0] *= s / CSCALE;
    (*c)[1] *= s / CSCALE;
}

// standard correlator ---------------------------------------------------------
static void corr_std(const sdr_cpx16_t *IQ, const sdr_cpx16_t *code, int N,
    const int *pos, int n, sdr_cpx_t *corr)
{
    for (int i = 0; i < n; i++) {
        if (pos[i] > 0) {
            int M = N - pos[i];
            dot_IQ_code(IQ + pos[i], code, M, 1.0f / M, corr + i);
        }
        else if (pos[i] < 0) {
            int M = N + pos[i];
            dot_IQ_code(IQ, code - pos[i], M, 1.0f / M, corr + i);
        }
        else {
            dot_IQ_code(IQ, code, N, 1.0f / N, corr + i);
        }
    }
}

// mix carrier and standard correlator -----------------------------------------
void sdr_corr_std(const sdr_buff_t *buff, int ix, int N, double fs, double fc,
    double phi, const sdr_cpx16_t *code, const int *pos, int n, sdr_cpx_t *corr)
{
    sdr_cpx16_t IQ[N];
    sdr_mix_carr(buff, ix, N, fs, fc, phi, IQ);
    corr_std(IQ, code, N, pos, n, corr);
}

// get FFTW plan ---------------------------------------------------------------
static int get_fftw_plan(int N, fftwf_plan *plan)
{
    static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
    
    pthread_mutex_lock(&mtx);
    
    for (int i = 0; i < MAX_FFTW_PLAN; i++) {
        if (fftw_size[i] == 0) {
            sdr_cpx_t *cpx1 = sdr_cpx_malloc(N);
            sdr_cpx_t *cpx2 = sdr_cpx_malloc(N);
            fftw_plans[i][0] = fftwf_plan_dft_1d(N, cpx1, cpx2, FFTW_FORWARD,  FFTW_FLAG);
            fftw_plans[i][1] = fftwf_plan_dft_1d(N, cpx2, cpx1, FFTW_BACKWARD, FFTW_FLAG);
            fftw_size[i] = N;
            sdr_cpx_free(cpx1);
            sdr_cpx_free(cpx2);
        }
        if (fftw_size[i] == N) {
            plan[0] = fftw_plans[i][0];
            plan[1] = fftw_plans[i][1];
            pthread_mutex_unlock(&mtx);
            return 1;
        }
    }
    fprintf(stderr, "fftw plan buffer overflow N=%d\n", N);
    pthread_mutex_unlock(&mtx);
    return 0;
}

// FFT correlator --------------------------------------------------------------
static void corr_fft(const sdr_cpx16_t *IQ, const sdr_cpx_t *code_fft, int N,
    sdr_cpx_t *corr)
{
    fftwf_plan plan[2];
    
    if (!get_fftw_plan(N, plan)) return;
    sdr_cpx_t *cpx = sdr_cpx_malloc(N * 2);
    for (int i = 0; i < N; i++) {
        cpx[i][0] = IQ[i].I / CSCALE;
        cpx[i][1] = IQ[i].Q / CSCALE;
    }
    // ifft(fft(data) * code_fft) / N^2 
    fftwf_execute_dft(plan[0], cpx, cpx + N);
    sdr_cpx_mul(cpx + N, code_fft, N, 1.0f / N / N, cpx);
    fftwf_execute_dft(plan[1], cpx, corr);
    
    sdr_cpx_free(cpx);
}

// mix carrier and FFT correlator ----------------------------------------------
void sdr_corr_fft(const sdr_buff_t *buff, int ix, int N, double fs, double fc,
    double phi, const sdr_cpx_t *code_fft, sdr_cpx_t *corr)
{
    sdr_cpx16_t IQ[N];
    sdr_mix_carr(buff, ix, N, fs, fc, phi, IQ);
    corr_fft(IQ, code_fft, N, corr);
}

// open log --------------------------------------------------------------------
int sdr_log_open(const char *path)
{
    const char *p = strchr(path, ':');
    int stat;
    
    if (!p || *(p + 1) == ':' ) { // file (path = file[::opt...])
        stat = stropen(&log_str, STR_FILE, STR_MODE_W, path);
    }
    else if (p == path) { // TCP server (path = :port)
        stat = stropen(&log_str, STR_TCPSVR, STR_MODE_W, path);
    }
    else { // TCP client (path = addr:port)
        stat = stropen(&log_str, STR_TCPCLI, STR_MODE_W, path);
    }
    if (!stat) {
        fprintf(stderr, "log stream open error %s\n", path);
    }
    return stat;
}

// close log -------------------------------------------------------------------
void sdr_log_close(void)
{
    strclose(&log_str);
}

// set log level ---------------------------------------------------------------
void sdr_log_level(int level)
{
    log_lvl = level;
}

// output log ------------------------------------------------------------------
void sdr_log(int level, const char *msg, ...)
{
    va_list ap;
    
    va_start(ap, msg);
    
    if (log_lvl == 0) {
        vprintf(msg, ap);
    }
    else if (level <= log_lvl) {
        char buff[1024];
        int len = vsnprintf(buff, sizeof(buff) - 2, msg, ap);
        len = MIN(len, (int)sizeof(buff) - 3);
        sprintf(buff + len, "\r\n");
        strwrite(&log_str, (uint8_t *)buff, len + 2);
    }
    va_end(ap);
}

// parse numbers list and range ------------------------------------------------
int sdr_parse_nums(const char *str, int *prns)
{
    int n = 0, prn, prn1, prn2;
    char buff[1024], *p, *q;
    
    sprintf(buff, "%*s", (int)sizeof(buff) - 1, str);
    
    for (p = buff; ; p = q + 1) {
        if ((q = strchr(p, ','))) {
            *q = '\0';
        }
        if (sscanf(p, "%d-%d", &prn1, &prn2) == 2) {
            for (prn = prn1; prn <= prn2 && n < SDR_MAX_NPRN; prn++) {
                prns[n++] = prn;
            }
        }
        else if (sscanf(p, "%d", &prn) == 1 && n < SDR_MAX_NPRN) {
             prns[n++] = prn;
        }
        if (!q) break;
    }
    return n;
}

// add item to buffer ----------------------------------------------------------
void sdr_add_buff(void *buff, int len_buff, void *item, size_t size_item)
{
    memmove(buff, buff + size_item, size_item * (len_buff - 1));
    memcpy(buff + size_item * (len_buff - 1), item, size_item);
}

// pack bit array to uint8_t array ---------------------------------------------
void sdr_pack_bits(const uint8_t *data, int nbit, int nz, uint8_t *buff)
{
    memset(buff, 0, (nz + nbit + 7) / 8);
    for (int i = nz; i < nz + nbit; i++) {
        buff[i / 8] |= data[i-nz] << (7 - i % 8);
    }
}

// unpack uint8_t array to bit array -------------------------------------------
void sdr_unpack_bits(const uint8_t *data, int nbit, uint8_t *buff)
{
    for (int i = 0; i < nbit * 8; i++) {
        buff[i] = (data[i / 8] >> (7 - i % 8)) & 1;
    }
}

// unpack data to bits ---------------------------------------------------------
void sdr_unpack_data(uint32_t data, int nbit, uint8_t *buff)
{
    for (int i = 0; i < nbit; i++) {
        buff[i] = (data >> (nbit - 1 - i)) & 1;
    }
}

// exclusive-or of all bits ----------------------------------------------------
uint8_t sdr_xor_bits(uint32_t X)
{
    static const uint8_t xor_8b[] = { // xor of 8 bits
        0,1,1,0,1,0,0,1, 1,0,0,1,0,1,1,0, 1,0,0,1,0,1,1,0, 0,1,1,0,1,0,0,1,
        1,0,0,1,0,1,1,0, 0,1,1,0,1,0,0,1, 0,1,1,0,1,0,0,1, 1,0,0,1,0,1,1,0,
        1,0,0,1,0,1,1,0, 0,1,1,0,1,0,0,1, 0,1,1,0,1,0,0,1, 1,0,0,1,0,1,1,0,
        0,1,1,0,1,0,0,1, 1,0,0,1,0,1,1,0, 1,0,0,1,0,1,1,0, 0,1,1,0,1,0,0,1,
        1,0,0,1,0,1,1,0, 0,1,1,0,1,0,0,1, 0,1,1,0,1,0,0,1, 1,0,0,1,0,1,1,0,
        0,1,1,0,1,0,0,1, 1,0,0,1,0,1,1,0, 1,0,0,1,0,1,1,0, 0,1,1,0,1,0,0,1,
        0,1,1,0,1,0,0,1, 1,0,0,1,0,1,1,0, 1,0,0,1,0,1,1,0, 0,1,1,0,1,0,0,1,
        1,0,0,1,0,1,1,0, 0,1,1,0,1,0,0,1, 0,1,1,0,1,0,0,1, 1,0,0,1,0,1,1,0
    };
    return xor_8b[(uint8_t)X] ^ xor_8b[(uint8_t)(X >> 8)] ^
        xor_8b[(uint8_t)(X >> 16)] ^ xor_8b[(uint8_t)(X >> 24)];
}

// generate FFTW wisdom --------------------------------------------------------
int sdr_gen_fftw_wisdom(const char *file, int N)
{
    fftwf_plan plan[2] = {0};
    
    sdr_cpx_t *cpx1 = sdr_cpx_malloc(N);
    sdr_cpx_t *cpx2 = sdr_cpx_malloc(N);
    plan[0] = fftwf_plan_dft_1d(N, cpx1, cpx2, FFTW_FORWARD,  FFTW_PATIENT);
    plan[1] = fftwf_plan_dft_1d(N, cpx2, cpx1, FFTW_BACKWARD, FFTW_PATIENT);
    
    int stat = fftwf_export_wisdom_to_filename(file);
    
    fftwf_destroy_plan(plan[0]);
    fftwf_destroy_plan(plan[1]);
    sdr_cpx_free(cpx1);
    sdr_cpx_free(cpx2);
    
    return stat;
}

