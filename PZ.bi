/**
 * Lotka-Volterra-like phytoplankton-zooplankton (PZ) model.
 * 
 * @author Lawrence Murray <lawrence.murray@csiro.au>
 * $Rev$
 * $Date$
 */
model PZ {
  const c = 0.25     // zooplankton clearance rate
  const e = 0.3      // zooplankton growth efficiency
  const m_l = 0.1    // zooplankton linear mortality
  const m_q = 0.1    // zooplankton quadratic mortality

  param EPg, VPg     // mean and standard deviation of phytoplankton growth
  state P, Z         // phytoplankton, zooplankton
  noise alpha        // stochastic phytoplankton growth rate
  obs P_obs          // observations of phytoplankton
  
  sub parameter {
    EPg ~ uniform(0.0, 1.0)
    VPg ~ uniform(0.0, 0.5)
  }
  
  sub proposal_parameter {
    EPg ~ gaussian(EPg, 0.02);
    VPg ~ gaussian(VPg, 0.01);
  }

  sub initial {
    P ~ log_normal(log(2.0), 0.2)
    Z ~ log_normal(log(2.0), 0.1)
  }

  sub transition(delta = 1.0) {
    do {
      alpha ~ gaussian(EPg, VPg)
    } then ode(atoler = 1.0e-3, rtoler = 1.0e-3, alg = 'rk43') {
      P <- ode(alpha*P - c*P*Z)
      Z <- ode(e*c*P*Z - m_l*Z - m_q*Z*Z)
    }
  }

  sub lookahead_transition {
    do {
      alpha <- 0
    } then ode(atoler = 1.0e-6, rtoler = 1.0e-3, alg = 'dopri5') {
      P <- ode(alpha*P - c*P*Z)
      Z <- ode(e*c*P*Z - m_l*Z - m_q*Z*Z)
    }
  }

  sub observation {
    P_obs ~ log_normal(log(P), 0.2)
  }
}