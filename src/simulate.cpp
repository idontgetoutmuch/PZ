/**
 * @file
 *
 * @author Lawrence Murray <lawrence.murray@csiro.au>
 * $Rev: 1248 $
 * $Date: 2011-01-31 16:28:43 +0800 (Mon, 31 Jan 2011) $
 */
#include "model/PZModel.hpp"

#include "bi/cuda/cuda.hpp"
#include "bi/math/ode.hpp"
#include "bi/random/Random.hpp"
#include "bi/updater/RUpdater.hpp"
#include "bi/method/Simulator.hpp"
#include "bi/buffer/SparseInputNetCDFBuffer.hpp"
#include "bi/buffer/SimulatorNetCDFBuffer.hpp"
#include "bi/misc/TicToc.hpp"

#include <iostream>
#include <string>
#include <unistd.h>
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
    SEED_ARG,
    INIT_FILE_ARG,
    FORCE_FILE_ARG,
    OUTPUT_FILE_ARG,
    OUTPUT_ARG,
    TIME_ARG,
    INCLUDE_PARAMETERS_ARG
  };
  real T = 0.0, H = 1.0, RTOLER = 1.0e-3, ATOLER = 1.0e-3;
  int P = 0, K = 0, INIT_NS = 0, FORCE_NS = 0, SEED = 0;
  std::string INIT_FILE, FORCE_FILE, OUTPUT_FILE;
  bool OUTPUT = false, TIME = false, INCLUDE_PARAMETERS = false;
  int c, option_index;

  option long_options[] = {
      {"atoler", required_argument, 0, ATOLER_ARG },
      {"rtoler", required_argument, 0, RTOLER_ARG },
      {"init-ns", required_argument, 0, INIT_NS_ARG },
      {"force-ns", required_argument, 0, FORCE_NS_ARG },
      {"seed", required_argument, 0, SEED_ARG },
      {"init-file", required_argument, 0, INIT_FILE_ARG },
      {"force-file", required_argument, 0, FORCE_FILE_ARG },
      {"output-file", required_argument, 0, OUTPUT_FILE_ARG },
      {"output", required_argument, 0, OUTPUT_ARG },
      {"time", required_argument, 0, TIME_ARG },
      {"include-parameters", required_argument, 0, INCLUDE_PARAMETERS_ARG }
  };
  const char* short_options = "T:P:K:h:";

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
    case SEED_ARG:
      SEED = atoi(optarg);
      break;
    case INIT_FILE_ARG:
      INIT_FILE = std::string(optarg);
      break;
    case FORCE_FILE_ARG:
      FORCE_FILE = std::string(optarg);
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
    case 'T':
      T = atof(optarg);
      break;
    case 'P':
      P = atoi(optarg);
      break;
    case 'K':
      K = atoi(optarg);
      break;
    case 'h':
      H = atof(optarg);
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

  /* random number generator */
  Random rng(SEED);

  /* model */
  model_type m;

  /* state */
  Static<LOCATION> theta(m, INCLUDE_PARAMETERS ? P : 1);
  State<LOCATION> s(m, P);

  /* inputs */
  SparseInputNetCDFBuffer *inForce = NULL, *inInit = NULL;
  if (!FORCE_FILE.empty()) {
    inForce = new SparseInputNetCDFBuffer(m, FORCE_FILE, FORCE_NS);
  }
  if (!INIT_FILE.empty()) {
    inInit = new SparseInputNetCDFBuffer(m, INIT_FILE, INIT_NS);
    inInit->read(P_NODE, theta.get(P_NODE));
    inInit->read(D_NODE, s.get(D_NODE));
    inInit->read(C_NODE, s.get(C_NODE));
  }

  /* output */
  SimulatorNetCDFBuffer* out = NULL;
  if (OUTPUT && !OUTPUT_FILE.empty()) {
    out = new SimulatorNetCDFBuffer(m, P, K, OUTPUT_FILE,
        NetCDFBuffer::REPLACE,
        INCLUDE_PARAMETERS ? STATIC_OWN : STATIC_SHARED);
  }

  /* initialise trajectories */
  m.getPrior<D_NODE>().samples(rng, s.get(D_NODE));
  m.getPrior<C_NODE>().samples(rng, s.get(C_NODE));
  if (INCLUDE_PARAMETERS) {
    m.getPrior<P_NODE>().samples(rng, theta.get(P_NODE));
  }

  /* simulate */
  RUpdater<model_type> rUpdater(rng);
  TicToc timer;
  if (INCLUDE_PARAMETERS) {
    BOOST_AUTO(sim, (SimulatorFactory<LOCATION,STATIC_OWN>::create(m, &rUpdater, inForce, out)));
    sim->simulate(T, theta, s);
    delete sim;
  } else {
    BOOST_AUTO(sim, (SimulatorFactory<LOCATION,STATIC_SHARED>::create(m, &rUpdater, inForce, out)));
    sim->simulate(T, theta, s);
    delete sim;
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
