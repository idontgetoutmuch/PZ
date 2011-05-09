/**
 * @file
 *
 * @author Lawrence Murray <lawrence.murray@csiro.au>
 * $Rev: 1251 $
 * $Date: 2011-01-31 18:40:46 +0800 (Mon, 31 Jan 2011) $
 */
#include "model/PZModel.hpp"

#include "bi/cuda/cuda.hpp"
#include "bi/math/ode.hpp"
#include "bi/random/Random.hpp"
#include "bi/method/StratifiedResampler.hpp"
#include "bi/method/KernelForwardBackwardSmoother.hpp"
#include "bi/buffer/ParticleFilterNetCDFBuffer.hpp"
#include "bi/buffer/ParticleSmootherNetCDFBuffer.hpp"
#include "bi/buffer/SparseInputNetCDFBuffer.hpp"
#include "bi/misc/TicToc.hpp"

#include <iostream>
#include <fstream>
#include <iomanip>
#include <string>
#include <cstdlib>
#include <sys/time.h>
#include <getopt.h>

#ifdef USE_CPU
#define LOCATION ON_HOST
#else
#define LOCATION ON_DEVICE
#endif

using namespace bi;

int main(int argc, char* argv[]) {
  /* model type */
  typedef PZModel<X_LEN,Y_LEN,Z_LEN> model_type;
  
  /* command line arguments */
  enum {
    ATOLER_ARG,
    RTOLER_ARG,
    FORCE_NS_ARG,
    SEED_ARG,
    INPUT_FILE_ARG,
    FORCE_FILE_ARG,
    OUTPUT_FILE_ARG,
    OUTPUT_ARG,
    TIME_ARG,
    INCLUDE_PARAMETERS_ARG
  };
  real H = 1.0, RTOLER = 1.0e-3, ATOLER = 1.0e-3;
  int FORCE_NS = 0, SEED = 0;
  real B;
  std::string INPUT_FILE, FORCE_FILE, OUTPUT_FILE;
  bool OUTPUT = false, TIME = false, INCLUDE_PARAMETERS = false;
  int c, option_index;

  option long_options[] = {
      {"atoler", required_argument, 0, ATOLER_ARG },
      {"rtoler", required_argument, 0, RTOLER_ARG },
      {"force-ns", required_argument, 0, FORCE_NS_ARG },
      {"seed", required_argument, 0, SEED_ARG },
      {"input-file", required_argument, 0, INPUT_FILE_ARG },
      {"force-file", required_argument, 0, FORCE_FILE_ARG },
      {"output-file", required_argument, 0, OUTPUT_FILE_ARG },
      {"output", required_argument, 0, OUTPUT_ARG },
      {"time", required_argument, 0, TIME_ARG },
      {"estimate-parameters", required_argument, 0, INCLUDE_PARAMETERS_ARG }
  };
  const char* short_options = "b:h:";

  do {
    c = getopt_long(argc, argv, short_options, long_options, &option_index);
    switch(c) {
    case ATOLER_ARG:
      ATOLER = atof(optarg);
      break;
    case RTOLER_ARG:
      RTOLER = atof(optarg);
      break;
    case FORCE_NS_ARG:
      FORCE_NS = atoi(optarg);
      break;
    case SEED_ARG:
      SEED = atoi(optarg);
      break;
    case INPUT_FILE_ARG:
      INPUT_FILE = std::string(optarg);
      break;
    case OUTPUT_FILE_ARG:
      OUTPUT_FILE = std::string(optarg);
      break;
    case OUTPUT_ARG:
      OUTPUT = atoi(optarg);
      break;
    case TIME_ARG:
      TIME = atoi(optarg);
      break;
    case INCLUDE_PARAMETERS_ARG:
      INCLUDE_PARAMETERS = atoi(optarg);
      break;
    case 'h':
      H = atof(optarg);
      break;
    case 'b':
      B = atof(optarg);
      break;
    }
  } while (c != -1);

  /* bi init */
  #ifdef __CUDACC__
  cudaThreadSetCacheConfig(cudaFuncCachePreferL1);
  #endif
  bi_omp_init();
  bi_ode_init(H, ATOLER, RTOLER);

  /* NetCDF error reporting */
  NcError ncErr(NcError::silent_nonfatal);

  /* model */
  model_type m;
  const int NP = m.getNetSize(P_NODE);
  const int ND = m.getNetSize(D_NODE);
  const int NC = m.getNetSize(C_NODE);
  const int N = ND + NC + (INCLUDE_PARAMETERS ? NP : 0);
  
  /* random number generator */
  Random rng(SEED);

  /* inputs */
  ParticleFilterNetCDFBuffer in(m, INPUT_FILE, NetCDFBuffer::READ_ONLY,
      INCLUDE_PARAMETERS ? STATIC_OWN : STATIC_SHARED);
  SparseInputNetCDFBuffer *inForce = NULL;
  if (!FORCE_FILE.empty()) {
    inForce = new SparseInputNetCDFBuffer(m, FORCE_FILE, FORCE_NS);
  }

  /* state and intermediate results */
  const int P = in.size1();
  const int T = in.size2();
  Static<LOCATION> theta(m, INCLUDE_PARAMETERS ? P : 1);
  State<LOCATION> s(m, P);

  /* output */
  ParticleSmootherNetCDFBuffer* out;
  if (OUTPUT) {
    out = new ParticleSmootherNetCDFBuffer(m, P, T, OUTPUT_FILE,
        NetCDFBuffer::REPLACE,
        INCLUDE_PARAMETERS ? STATIC_OWN : STATIC_SHARED);
  } else {
    out = NULL;
  }

  /* smooth */
  FastGaussianKernel K(N, B);
  MedianPartitioner partitioner;
  StratifiedResampler resam(rng);
  TicToc timer;
  if (INCLUDE_PARAMETERS) {
    BOOST_AUTO(smoother, (KernelForwardBackwardSmootherFactory<LOCATION,STATIC_OWN>::create(m, rng, K, partitioner, inForce, out)));
    smoother->smooth(theta, s, &in, &resam);
    delete smoother;
  } else {
    BOOST_AUTO(smoother, (KernelForwardBackwardSmootherFactory<LOCATION,STATIC_SHARED>::create(m, rng, K, partitioner, inForce, out)));
    smoother->smooth(theta, s, &in, &resam);
    delete smoother;
  }

  /* output timing results */
  if (TIME) {
    std::cout << timer.toc() << std::endl;
  }

  delete out;

  return 0;
}
