/**
 * @file
 *
 * @author Lawrence Murray <lawrence.murray@csiro.au>
 * $Rev: 1335 $
 * $Date: 2011-03-29 12:15:09 +0800 (Tue, 29 Mar 2011) $
 */
#include "model/PZModel.hpp"

#include "bi/cuda/cuda.hpp"
#include "bi/math/ode.hpp"
#include "bi/random/Random.hpp"
#include "bi/method/ParticleFilter.hpp"
#include "bi/method/AuxiliaryParticleFilter.hpp"
#include "bi/method/DisturbanceParticleFilter.hpp"
#include "bi/method/ExactStratifiedResampler.hpp"
#include "bi/method/StratifiedResampler.hpp"
#include "bi/method/MultinomialResampler.hpp"
#include "bi/method/MetropolisResampler.hpp"
#include "bi/buffer/ParticleFilterNetCDFBuffer.hpp"
#include "bi/buffer/SparseInputNetCDFBuffer.hpp"
#include "bi/misc/TicToc.hpp"

#include "boost/typeof/typeof.hpp"

#include <iostream>
#include <iomanip>
#include <string>
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
    INIT_NS_ARG,
    FORCE_NS_ARG,
    OBS_NS_ARG,
    SEED_ARG,
    INIT_FILE_ARG,
    FORCE_FILE_ARG,
    OBS_FILE_ARG,
    OUTPUT_FILE_ARG,
    RESAMPLER_ARG,
    OUTPUT_ARG,
    TIME_ARG,
    INCLUDE_PARAMETERS_ARG
  };
  real T = 0.0, H = 1.0, RTOLER = 1.0e-3, ATOLER = 1.0e-3;
  int P = 1024, L = 10, INIT_NS = 0, FORCE_NS = 0, OBS_NS = 0, SEED = 0;
  std::string INIT_FILE, FORCE_FILE, OBS_FILE, OUTPUT_FILE, RESAMPLER = std::string("stratified");
  bool OUTPUT = false, TIME = false, INCLUDE_PARAMETERS = false;
  int c, option_index;

  option long_options[] = {
      {"atoler", required_argument, 0, ATOLER_ARG },
      {"rtoler", required_argument, 0, RTOLER_ARG },
      {"init-ns", required_argument, 0, INIT_NS_ARG },
      {"force-ns", required_argument, 0, FORCE_NS_ARG },
      {"obs-ns", required_argument, 0, OBS_NS_ARG },
      {"seed", required_argument, 0, SEED_ARG },
      {"init-file", required_argument, 0, INIT_FILE_ARG },
      {"force-file", required_argument, 0, FORCE_FILE_ARG },
      {"obs-file", required_argument, 0, OBS_FILE_ARG },
      {"output-file", required_argument, 0, OUTPUT_FILE_ARG },
      {"resampler", required_argument, 0, RESAMPLER_ARG },
      {"output", required_argument, 0, OUTPUT_ARG },
      {"time", required_argument, 0, TIME_ARG },
      {"include-parameters", required_argument, 0, INCLUDE_PARAMETERS_ARG }
  };
  const char* short_options = "T:h:P:L:";

  do {
    c = getopt_long(argc, argv, short_options, long_options, &option_index);
    switch(c) {
    case ATOLER_ARG:
      ATOLER = atof(optarg);
      break;
    case RTOLER_ARG:
      RTOLER = atof(optarg);
      break;
    case INIT_NS_ARG:
      INIT_NS = atoi(optarg);
      break;
    case FORCE_NS_ARG:
      FORCE_NS = atoi(optarg);
      break;
    case OBS_NS_ARG:
      OBS_NS = atoi(optarg);
      break;
    case SEED_ARG:
      SEED = atoi(optarg);
      break;
    case INIT_FILE_ARG:
      INIT_FILE = std::string(optarg);
      break;
    case FORCE_FILE_ARG:
      FORCE_FILE = std::string(optarg);
      break;
    case OBS_FILE_ARG:
      OBS_FILE = std::string(optarg);
      break;
    case OUTPUT_FILE_ARG:
      OUTPUT_FILE = std::string(optarg);
      break;
    case RESAMPLER_ARG:
      RESAMPLER = std::string(optarg);
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
    case 'T':
      T = atof(optarg);
      break;
    case 'h':
      H = atof(optarg);
      break;
    case 'P':
      P = atoi(optarg);
      break;
    case 'L':
      L = atoi(optarg);
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

  /* state and intermediate results */
  Static<LOCATION> theta(m, INCLUDE_PARAMETERS ? P : 1);
  State<LOCATION> s(m, P);

  /* random number generator */
  Random rng(SEED);

  /* inputs */
  SparseInputNetCDFBuffer inObs(m, OBS_FILE, OBS_NS);
  SparseInputNetCDFBuffer *inForce = NULL, *inInit = NULL;
  if (!FORCE_FILE.empty()) {
    inForce = new SparseInputNetCDFBuffer(m, FORCE_FILE, FORCE_NS);
  }
  if (!INIT_FILE.empty()) {
    inInit = new SparseInputNetCDFBuffer(m, INIT_FILE, INIT_NS);
    inInit->read(P_NODE, theta.get(P_NODE));
    inInit->read(D_NODE, s.get(D_NODE));
    inInit->read(C_NODE, s.get(C_NODE));
  } else {
    m.getPrior<D_NODE>().samples(rng, s.get(D_NODE));
    m.getPrior<C_NODE>().samples(rng, s.get(C_NODE));
    if (INCLUDE_PARAMETERS) {
      m.getPrior<P_NODE>().samples(rng, theta.get(P_NODE));      
    }
  }

  /* output */
  ParticleFilterNetCDFBuffer* out;
  if (OUTPUT) {
    out = new ParticleFilterNetCDFBuffer(m, P, inObs.countUniqueTimes(T),
        OUTPUT_FILE, NetCDFBuffer::REPLACE,
        INCLUDE_PARAMETERS ? STATIC_OWN : STATIC_SHARED);
  } else {
    out = NULL;
  }

  /* set filter */
  //ExactStratifiedResampler resam(rng);
  StratifiedResampler resam(rng);

  /* do filter */
  TicToc timer;
  if (INCLUDE_PARAMETERS) {
    BOOST_AUTO(filter, (ParticleFilterFactory<LOCATION,STATIC_OWN>::create(m, rng, inForce, &inObs, out)));
    filter->filter(T, theta, s, &resam);
    delete filter;
  } else {
    BOOST_AUTO(filter, (ParticleFilterFactory<LOCATION,STATIC_SHARED>::create(m, rng, inForce, &inObs, out)));
    filter->filter(T, theta, s, &resam);
    delete filter;
  }
  
  /* output timing results */
  if (TIME) {
    synchronize();
    std::cout << timer.toc() << std::endl;
  }

  delete out;
  delete inForce;
  delete inInit;

  return 0;
}
