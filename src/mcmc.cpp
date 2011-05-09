/**
 * @file
 *
 * @author Lawrence Murray <lawrence.murray@csiro.au>
 * $Rev: 1274 $
 * $Date: 2011-02-18 12:29:09 +0800 (Fri, 18 Feb 2011) $
 */
#include "device.hpp"
#include "model/PZModel.hpp"

#include "bi/cuda/cuda.hpp"
#include "bi/math/ode.hpp"
#include "bi/state/State.hpp"
#include "bi/random/Random.hpp"
#include "bi/pdf/AdditiveExpGaussianPdf.hpp"
#include "bi/pdf/ExpGaussianMixturePdf.hpp"
#include "bi/method/ParticleMCMC.hpp"
#include "bi/method/ParticleFilter.hpp"
#include "bi/method/StratifiedResampler.hpp"
#include "bi/buffer/ParticleFilterNetCDFBuffer.hpp"
#include "bi/buffer/ParticleMCMCNetCDFBuffer.hpp"
#include "bi/buffer/SparseInputNetCDFBuffer.hpp"
#include "bi/buffer/UnscentedRTSSmootherNetCDFBuffer.hpp"

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
  /* command line arguments */
  enum {
    ID_ARG,
    ATOLER_ARG,
    RTOLER_ARG,
    SCALE_ARG,
    SD_ARG,
    INIT_NS_ARG,
    FORCE_NS_ARG,
    OBS_NS_ARG,
    SEED_ARG,
    INIT_FILE_ARG,
    FORCE_FILE_ARG,
    OBS_FILE_ARG,
    OUTPUT_FILE_ARG,
    FILTER_FILE_ARG,
    PROPOSAL_FILE_ARG,
    RESAMPLER_ARG
  };
  real T = 0.0, H = 1.0, RTOLER = 1.0e-3, ATOLER = 1.0e-3,
      SCALE = 0.01, SD = 0.0;
  int ID = 0, P = 1024, L = 10, INIT_NS = 0, FORCE_NS = 0, OBS_NS = 0,
      SEED = 0, C = 100, A = 1000;
  std::string INIT_FILE, FORCE_FILE, OBS_FILE, FILTER_FILE, OUTPUT_FILE,
      PROPOSAL_FILE, RESAMPLER = std::string("stratified");
  int c, option_index;

  option long_options[] = {
      {"id", required_argument, 0, ID_ARG },
      {"atoler", required_argument, 0, ATOLER_ARG },
      {"rtoler", required_argument, 0, RTOLER_ARG },
      {"sd", required_argument, 0, SD_ARG },
      {"scale", required_argument, 0, SCALE_ARG },
      {"init-ns", required_argument, 0, INIT_NS_ARG },
      {"force-ns", required_argument, 0, FORCE_NS_ARG },
      {"obs-ns", required_argument, 0, OBS_NS_ARG },
      {"seed", required_argument, 0, SEED_ARG },
      {"init-file", optional_argument, 0, INIT_FILE_ARG },
      {"force-file", required_argument, 0, FORCE_FILE_ARG },
      {"obs-file", required_argument, 0, OBS_FILE_ARG },
      {"filter-file", required_argument, 0, FILTER_FILE_ARG },
      {"proposal-file", optional_argument, 0, PROPOSAL_FILE_ARG },
      {"output-file", required_argument, 0, OUTPUT_FILE_ARG },
      {"resampler", required_argument, 0, RESAMPLER_ARG }
  };
  const char* short_options = "T:h:P:L:C:A:";

  do {
    c = getopt_long(argc, argv, short_options, long_options, &option_index);
    switch(c) {
    case ID_ARG:
      ID = atoi(optarg);
      break;
    case ATOLER_ARG:
      ATOLER = atof(optarg);
      break;
    case RTOLER_ARG:
      RTOLER = atof(optarg);
      break;
    case SD_ARG:
      SD = atof(optarg);
      break;
    case SCALE_ARG:
      SCALE = atof(optarg);
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
      if (optarg) {
        INIT_FILE = std::string(optarg);
      }
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
    case FILTER_FILE_ARG:
      FILTER_FILE = std::string(optarg);
      break;
    case PROPOSAL_FILE_ARG:
      if (optarg) {
        PROPOSAL_FILE = std::string(optarg);
      }
      break;
    case RESAMPLER_ARG:
      RESAMPLER = std::string(optarg);
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
    case 'C':
      C = atoi(optarg);
      break;
    case 'A':
      A = atoi(optarg);
      break;
    }
  } while (c != -1);

  /* bi init */
  #ifdef __CUDACC__
  chooseDevice(ID);
  cudaThreadSetCacheConfig(cudaFuncCachePreferL1);
  #endif
  bi_omp_init();
  bi_ode_init(H, ATOLER, RTOLER);

  /* NetCDF error reporting */
  NcError ncErr(NcError::silent_nonfatal);

  /* random number generator */
  Random rng(SEED);

  /* model */
  PZModel<> m;
  const int NP = m.getNetSize(P_NODE);
  const int ND = m.getNetSize(D_NODE);
  const int NC = m.getNetSize(C_NODE);
  if (SD <= 0.0) {
    SD = 2.4*2.4/NP;
  }

  /* state and intermediate results */
  Static<LOCATION> theta(m);
  State<LOCATION> s(m, P);

  /* inputs and outputs */
  SparseInputNetCDFBuffer *inForce = NULL, *inInit = NULL;
  if (!FORCE_FILE.empty()) {
    inForce = new SparseInputNetCDFBuffer(m, FORCE_FILE, FORCE_NS);
  }
  if (!INIT_FILE.empty()) {
    inInit = new SparseInputNetCDFBuffer(m, INIT_FILE, INIT_NS);
    inInit->read(P_NODE, theta.get(P_NODE));
  }
  SparseInputNetCDFBuffer inObs(m, OBS_FILE, OBS_NS);
  const int Y = inObs.countUniqueTimes(T);
  ParticleMCMCNetCDFBuffer out(m, C, Y, OUTPUT_FILE, NetCDFBuffer::REPLACE);
  ParticleFilterNetCDFBuffer outFilter(m, P, Y, FILTER_FILE, NetCDFBuffer::REPLACE);

  /* filter */
  StratifiedResampler resam(rng);
  BOOST_AUTO(filter, ParticleFilterFactory<LOCATION>::create(m, rng, inForce, &inObs, &outFilter));

  /* sampler */
  BOOST_AUTO(mcmc, ParticleMCMCFactory<LOCATION>::create(m, rng, &out, INITIAL_CONDITIONED));
  
  /* proposal */
  BOOST_AUTO(p0, mcmc->getPrior()); // prior
  AdditiveExpGaussianPdf<> q(p0.size());
  host_vector<real> x(p0.size());
  
  if (!PROPOSAL_FILE.empty()) {
    /* construct from online posterior */
    ExpGaussianPdf<> p1(p0.size()); // online posterior
    UnscentedRTSSmootherNetCDFBuffer inProposal(m, PROPOSAL_FILE,
        NetCDFBuffer::READ_ONLY, STATIC_OWN);   
    inProposal.readSmoothState(0, p1.mean(), p1.cov());
    p1.addLogs(m.getPrior(D_NODE).getLogs(), 0);
    p1.addLogs(m.getPrior(C_NODE).getLogs(), ND);
    p1.addLogs(m.getPrior(P_NODE).getLogs(), ND + NC);
    p1.init();
    
    /* initialise chain */
    p1.sample(rng, x);
    
    /* initialise proposal */
    q.cov() = p1.cov();
    q.addLogs(m.getPrior(D_NODE).getLogs(), 0);
    q.addLogs(m.getPrior(C_NODE).getLogs(), ND);
    q.addLogs(m.getPrior(P_NODE).getLogs(), ND + NC);
    matrix_scal(SD, q.cov());
  } else {
    /* construct from scaled prior */
    /* initialise chain */
    p0.sample(rng, x);

    /* initialise proposal */
    subrange(q.cov(), 0, ND, 0, ND) = subrange(m.getPrior(D_NODE).cov(), 0, ND, 0, ND);
    subrange(q.cov(), ND, NC, ND, NC) = subrange(m.getPrior(C_NODE).cov(), ND, NC, ND, NC);
    subrange(q.cov(), ND + NC, NP, ND + NC, NP) = subrange(m.getPrior(P_NODE).cov(), ND + NC, NP, ND + NC, NP);
    q.addLogs(m.getPrior(D_NODE).getLogs(), 0);
    q.addLogs(m.getPrior(C_NODE).getLogs(), ND);
    q.addLogs(m.getPrior(P_NODE).getLogs(), ND + NC);
    matrix_scal(SCALE, q.cov());
  }
  q.init();
  
  /* sample */
  mcmc->sample(q, x, C, T, theta, s, filter, &resam, SD, A);

  /* wrap up */
  std::cout << mcmc->getNumAccepted() << " of " << mcmc->getNumSteps() <<
      " proposals accepted" << std::endl;

  delete inForce;
  delete inInit;
  delete mcmc;
  delete filter;

  return 0;
}
