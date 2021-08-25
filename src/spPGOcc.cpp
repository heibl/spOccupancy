#include <string>
#include <R.h>
#include <Rmath.h>
#include <Rinternals.h>
#include <R_ext/Linpack.h>
#include <R_ext/Lapack.h>
#include <R_ext/BLAS.h>
#include "util.h"
#include "rpg.h"

// Optionally include OPENMP for parallelization if it exists. 
#ifdef _OPENMP
#include <omp.h>
#endif

// extern "C" means we are using C naming and calling conventions. This 
// allows us to write C++ code, but is necessary for linking back to R. 
extern "C" {
  // SEXP is the type all R objects are stored at the C-level.  
  SEXP spPGOcc(SEXP y_r, SEXP X_r, SEXP Xp_r, SEXP coordsD_r, SEXP pocc_r, SEXP pdet_r, 
	       SEXP J_r, SEXP K_r, SEXP betaStarting_r, SEXP alphaStarting_r, 
	       SEXP zStarting_r, SEXP wStarting_r, SEXP phiStarting_r, 
	       SEXP sigmaSqStarting_r, SEXP nuStarting_r, 
	       SEXP zLongIndx_r, SEXP muBeta_r, SEXP muAlpha_r, 
	       SEXP SigmaBeta_r, SEXP SigmaAlpha_r, SEXP phiA_r, SEXP phiB_r, 
	       SEXP sigmaSqA_r, SEXP sigmaSqB_r, SEXP nuA_r, SEXP nuB_r, 
	       SEXP tuning_r, SEXP covModel_r, SEXP nBatch_r, 
	       SEXP batchLength_r, SEXP acceptRate_r, SEXP nThreads_r, SEXP verbose_r, 
	       SEXP nReport_r){
   
    /**********************************************************************
     * Initial constants
     * *******************************************************************/
    int i, j, k, s, r, q, info, nProtect=0;
    int status = 0; // For AMCMC. 
    const int inc = 1;
    const double one = 1.0;
    const double negOne = -1.0;
    const double zero = 0.0;
    // These are specified as pointers since functions require the addresses.
    char const *lower = "L";
    char const *upper = "U";
    char const *ntran = "N";
    char const *ytran = "T";
    char const *rside = "R";
    char const *lside = "L";

    
    /**********************************************************************
     * Get Inputs
     * *******************************************************************/
    // The REAL or INTEGER are helper functions that allow you to access
    // the C array inside the R objects that are read in as inputs or 
    // created in the function. 
    double *y = REAL(y_r);
    double *X = REAL(X_r);
    double *coordsD = REAL(coordsD_r); 
    // Xp is sorted by parameter then site
    double *Xp = REAL(Xp_r);
    // Priors
    double *muBeta = REAL(muBeta_r); 
    double *muAlpha = REAL(muAlpha_r); 
    double *SigmaBetaInv = REAL(SigmaBeta_r); 
    double *SigmaAlphaInv = REAL(SigmaAlpha_r); 
    double phiA = REAL(phiA_r)[0];
    double phiB = REAL(phiB_r)[0]; 
    double nuA = REAL(nuA_r)[0]; 
    double nuB = REAL(nuB_r)[0]; 
    double sigmaSqA = REAL(sigmaSqA_r)[0]; 
    double sigmaSqB = REAL(sigmaSqB_r)[0]; 
    double *tuning = REAL(tuning_r); 
    int covModel = INTEGER(covModel_r)[0];
    std::string corName = getCorName(covModel);
    int pOcc = INTEGER(pocc_r)[0];
    int pDet = INTEGER(pdet_r)[0];
    int J = INTEGER(J_r)[0];
    int *K = INTEGER(K_r); 
    int *zLongIndx = INTEGER(zLongIndx_r); 
    int nObs = 0;
    for (j = 0; j < J; j++) {
      nObs += K[j]; 
    } // j
    int nBatch = INTEGER(nBatch_r)[0]; 
    int batchLength = INTEGER(batchLength_r)[0]; 
    int nSamples = nBatch * batchLength; 
    double acceptRate = REAL(acceptRate_r)[0];
    int nThreads = INTEGER(nThreads_r)[0];
    int verbose = INTEGER(verbose_r)[0];
    // z starting values 
    double *z = REAL(zStarting_r); 
    int nReport = INTEGER(nReport_r)[0];

// For parallelization.  
#ifdef _OPENMP
    omp_set_num_threads(nThreads);
#else
    if(nThreads > 1){
      warning("n.omp.threads > %i, but source not compiled with OpenMP support.", nThreads);
      nThreads = 1;
    }
#endif
    
    /**********************************************************************
     * Print Information 
     * *******************************************************************/
    if(verbose){
      // Rprintf allows you to print messages and value on the R console screen. 
      Rprintf("----------------------------------------\n");
      Rprintf("\tModel description\n");
      Rprintf("----------------------------------------\n");
      Rprintf("Spatial Occupancy Model with Polya-Gamma latent\nvariable fit with %i sites.\n\n", J);
      Rprintf("Number of MCMC samples %i (%i batches of length %i)\n\n", nSamples, nBatch, batchLength);
      Rprintf("Using the %s spatial correlation model.\n\n", corName.c_str());
#ifdef _OPENMP
      Rprintf("Source compiled with OpenMP support and model fit using %i thread(s).\n\n", nThreads);
#else
      Rprintf("Source not compiled with OpenMP support.\n\n");
#endif
      Rprintf("Adaptive Metropolis with target acceptance rate: %.1f\n", 100*acceptRate);
      Rprintf("Sampling ... \n");

    }

    /**********************************************************************
     * Parameters
     * *******************************************************************/
    // Occupancy covariates
    double *beta = (double *) R_alloc(pOcc, sizeof(double));   
    // This copies the starting values provided as user input into beta.  
    F77_NAME(dcopy)(&pOcc, REAL(betaStarting_r), &inc, beta, &inc);
    // Detection covariates
    double *alpha = (double *) R_alloc(pDet, sizeof(double));   
    F77_NAME(dcopy)(&pDet, REAL(alphaStarting_r), &inc, alpha, &inc);
    // Spatial random effects
    double *w = (double *) R_alloc(J, sizeof(double));   
    F77_NAME(dcopy)(&J, REAL(wStarting_r), &inc, w, &inc);
    // Spatial smooth parameter for matern. 
    double nu = REAL(nuStarting_r)[0]; 
    // Auxiliary variables
    double *omegaDet = (double *) R_alloc(nObs, sizeof(double));
    double *omegaOcc = (double *) R_alloc(J, sizeof(double));
    double *kappaDet = (double *) R_alloc(nObs, sizeof(double)); 
    double *kappaOcc = (double *) R_alloc(J, sizeof(double)); 
    

    /**********************************************************************
     * Return Stuff
     * *******************************************************************/
    SEXP betaSamples_r;
    // Create an R-level matrix. The PROTECT is necessary to ensure that 
    // the R objects you want for output are not deleted even if the garbage
    // collector is activated. 
    // The nProtect is used to track the number of protected objects, which 
    // is added to as additional objects are protected. 
    PROTECT(betaSamples_r = allocMatrix(REALSXP, pOcc, nSamples)); nProtect++;
    SEXP alphaSamples_r; 
    PROTECT(alphaSamples_r = allocMatrix(REALSXP, pDet, nSamples)); nProtect++;
    SEXP zSamples_r; 
    PROTECT(zSamples_r = allocMatrix(REALSXP, J, nSamples)); nProtect++; 
    SEXP wSamples_r; 
    PROTECT(wSamples_r = allocMatrix(REALSXP, J, nSamples)); nProtect++; 
    SEXP psiSamples_r; 
    PROTECT(psiSamples_r = allocMatrix(REALSXP, J, nSamples)); nProtect++; 
    SEXP yRepSamples_r; 
    PROTECT(yRepSamples_r = allocMatrix(INTSXP, nObs, nSamples)); nProtect++; 
    
    /**********************************************************************
     * Other initial starting stuff
     * *******************************************************************/
    int ppDet = pDet * pDet;
    int ppOcc = pOcc * pOcc; 
    int JpOcc = J * pOcc; 
    int JJ = J * J; 
    int nObspDet = nObs * pDet;
    // R_alloc is used to allocate memory. 
    // The memory allocated with R_alloc is automatically released when 
    // R returns from .Call. 
    // R_alloc is used when one wants to rrepresent native c data types
    // rather than R objects. 
    double *tmp_ppDet = (double *) R_alloc(ppDet, sizeof(double));
    double *tmp_ppOcc = (double *) R_alloc(ppOcc, sizeof(double)); 
    double *tmp_pDet = (double *) R_alloc(pDet, sizeof(double));
    double *tmp_pOcc = (double *) R_alloc(pOcc, sizeof(double));
    double *tmp_pDet2 = (double *) R_alloc(pDet, sizeof(double));
    double *tmp_pOcc2 = (double *) R_alloc(pOcc, sizeof(double));
    double *tmp_one = (double *) R_alloc(1, sizeof(double)); 
    double * tmp_JJ = (double *) R_alloc(JJ, sizeof(double)); 
    int *tmp_J = (int *) R_alloc(J, sizeof(int));
    for (j = 0; j < J; j++) {
      tmp_J[j] = zero; 
    }
    double *tmp_nObs = (double *) R_alloc(nObs, sizeof(double)); 
    double *tmp_JpOcc = (double *) R_alloc(JpOcc, sizeof(double));
    double *tmp_nObspDet = (double *) R_alloc(nObspDet, sizeof(double));
    double *tmp_J1 = (double *) R_alloc(J, sizeof(double));
   
    // For latent occupancy
    double psiNum; 
    double psiNew; 
    double *detProb = (double *) R_alloc(nObs, sizeof(double)); 
    double *psi = (double *) R_alloc(J, sizeof(double)); 
    double *piProd = (double *) R_alloc(J, sizeof(double)); 
    int *ySum = (int *) R_alloc(J, sizeof(int)); 
    int *yRep = (int *) R_alloc(nObs, sizeof(int)); 

    // For normal priors
    // Occupancy regression coefficient priors. 
    // Compute cholesky
    F77_NAME(dpotrf)(lower, &pOcc, SigmaBetaInv, &pOcc, &info); 
    if(info != 0){error("c++ error: dpotrf SigmaBetaInv failed\n");}
    // Compute inverse
    F77_NAME(dpotri)(lower, &pOcc, SigmaBetaInv, &pOcc, &info); 
    if(info != 0){error("c++ error: dpotri SigmaBetaInv failed\n");}
    double *SigmaBetaInvMuBeta = (double *) R_alloc(pOcc, sizeof(double)); 
    // dgemv computes linear combinations of different variables. 
    F77_NAME(dgemv)(ytran, &pOcc, &pOcc, &one, SigmaBetaInv, &pOcc, muBeta, &inc, &zero, SigmaBetaInvMuBeta, &inc); 	  
    // Detection regression coefficient priors. 
    // Compute cholesky
    F77_NAME(dpotrf)(lower, &pDet, SigmaAlphaInv, &pDet, &info); 
    if(info != 0){error("c++ error: dpotrf SigmaAlphaInv failed\n");}
    // Compute inverse
    F77_NAME(dpotri)(lower, &pDet, SigmaAlphaInv, &pDet, &info); 
    if(info != 0){error("c++ error: dpotri SigmaAlphaInv failed\n");}
    double *SigmaAlphaInvMuAlpha = (double *) R_alloc(pOcc, sizeof(double)); 
    F77_NAME(dgemv)(ytran, &pDet, &pDet, &one, SigmaAlphaInv, &pDet, muAlpha, &inc, &zero, SigmaAlphaInvMuAlpha, &inc); 	  

    /**********************************************************************
     * Set up spatial stuff and MH stuff
     * *******************************************************************/
    int nTheta, sigmaSqIndx, phiIndx, nuIndx;
    if (corName != "matern") {
      nTheta = 2; // sigma^2, phi 
      sigmaSqIndx = 0; phiIndx = 1; 
    } else {
      nTheta = 3; // sigma^2, phi, nu 
      sigmaSqIndx = 0; phiIndx = 1; nuIndx = 2; 
    }  
    double *accept = (double *) R_alloc(nTheta, sizeof(double)); zeros(accept, nTheta); 
    double *theta = (double *) R_alloc(nTheta, sizeof(double));
    double logMHRatio, logPostCurr = 0.0, logPostCand = 0.0, detCand = 0.0, detCurr = 0.0;
    double logDet;  
    double phiCand = 0.0, nuCand = 0.0;  
    SEXP acceptSamples_r; 
    PROTECT(acceptSamples_r = allocMatrix(REALSXP, nTheta, nBatch)); nProtect++; 
    SEXP tuningSamples_r; 
    PROTECT(tuningSamples_r = allocMatrix(REALSXP, nTheta, nBatch)); nProtect++; 
    SEXP thetaSamples_r; 
    PROTECT(thetaSamples_r = allocMatrix(REALSXP, nTheta, nSamples)); nProtect++; 
    // Initiate spatial values
    theta[sigmaSqIndx] = REAL(sigmaSqStarting_r)[0]; 
    double phi = REAL(phiStarting_r)[0]; 
    theta[phiIndx] = phi; 
    if (corName == "matern") {
      theta[nuIndx] = nu; 
    }
    double *C = (double *) R_alloc(JJ, sizeof(double));
    double *CCand = (double *) R_alloc(JJ, sizeof(double));
    double *tmp_JD = (double *) R_alloc(J, sizeof(double));
    double *tmp_JD2 = (double *) R_alloc(J, sizeof(double));
    double *R = (double *) R_alloc(JJ, sizeof(double)); 
    // Get spatial correlation matrix
    spCorLT(coordsD, J, theta, corName, R); 
    logPostCurr = R_NegInf; 
    // Get spatial covariance matrix 
    spCovLT(coordsD, J, theta, corName, C); 
    // Get cholesky of C
    F77_NAME(dpotrf)(lower, &J, C, &J, &info); 
    if(info != 0){error("c++ error: Cholesky failed in initial covariance matrix\n");}
    // Get inverse Cholesky of C. 
    F77_NAME(dpotri)(lower, &J, C, &J, &info); 
    if(info != 0){error("c++ error: Cholesky inverse failed in initial covariance matrix\n");}
    // C now contains the inverse of the covariance matrix. 
    // For sigmaSq sampler
    double aSigmaSqPost = 0.5 * J + sigmaSqA; 
    double bSigmaSqPost = 0.0; 
    double *wTRInv = (double *) R_alloc(J, sizeof(double)); 

    // This is necessary for generating random numbers in C 
    GetRNGstate();
   
    /**********************************************************************
     * Begin Sampler 
     * *******************************************************************/
    for (s = 0, q = 0; s < nBatch; s++) {
      for (r = 0; r < batchLength; r++, q++) {
    // for (s = 0, q = 0; s < 1; s++) {
    //   for (r = 0; r < 10; r++, q++) {
        /********************************************************************
         *Update Occupancy Auxiliary Variables 
         *******************************************************************/
        for (j = 0; j < J; j++) {
          omegaOcc[j] = rpg(1.0, F77_NAME(ddot)(&pOcc, &X[j], &J, beta, &inc) + w[j]);
        } // j
        /********************************************************************
         *Update Detection Auxiliary Variables 
         *******************************************************************/
        // Note that all of the variables are sampled, but only those at 
        // locations with z[j] == 1 actually effect the results. 
        for (i = 0; i < nObs; i++) {
          omegaDet[i] = rpg(1.0, F77_NAME(ddot)(&pDet, &Xp[i], &nObs, alpha, &inc));
        } // i
             
        /********************************************************************
         *Update Occupancy Regression Coefficients
         *******************************************************************/
        for (j = 0; j < J; j++) {
          kappaOcc[j] = z[j] - 1.0 / 2.0; 
	  tmp_J1[j] = kappaOcc[j] - omegaOcc[j] * w[j]; 
        } // j
        /********************************
         * Compute b.beta
         *******************************/
        // X * tmp_J1 + 0 * tmp_p. Output is stored in tmp_p
        // dgemv computes linear combinations of different variables. 
        F77_NAME(dgemv)(ytran, &J, &pOcc, &one, X, &J, tmp_J1, &inc, &zero, tmp_pOcc, &inc); 	 
        for (j = 0; j < pOcc; j++) {
          tmp_pOcc[j] += SigmaBetaInvMuBeta[j]; 
        } // j 

        /********************************
         * Compute A.beta
         * *****************************/
        // tmp_JpOcc is X %*% omegaOcc. 
        for(j = 0; j < J; j++){
          for(i = 0; i < pOcc; i++){
            tmp_JpOcc[i*J+j] = X[i*J+j]*omegaOcc[j];
          }
        }

        // This finishes off A.beta
        // 1 * X * tmp_JpOcc + 0 * tmp_ppOcc = tmp_ppOcc
        F77_NAME(dgemm)(ytran, ntran, &pOcc, &pOcc, &J, &one, X, &J, tmp_JpOcc, &J, &zero, tmp_ppOcc, &pOcc);
        for (j = 0; j < ppOcc; j++) {
          tmp_ppOcc[j] += SigmaBetaInv[j]; 
        } // j

        // This gives the Cholesky of A.beta
        // Computes cholesky of tmp_ppOcc. Output stored in tmp_ppOcc
        F77_NAME(dpotrf)(lower, &pOcc, tmp_ppOcc, &pOcc, &info); 
        if(info != 0){error("c++ error: dpotrf here failed\n");}
        // Computes the inverse tmp_ppOcc. Stored in tmp_ppOcc. This is A.beta.inv. 
        F77_NAME(dpotri)(lower, &pOcc, tmp_ppOcc, &pOcc, &info); 
        if(info != 0){error("c++ error: dpotri here failed\n");}
        // A.beta.inv %*% b.beta
        // 1 * tmp_ppOcc * tmp_pOcc + 0 * tmp_pOcc2 
        // (which is currently nothing) = tmp_pOcc2
        F77_NAME(dsymv)(lower, &pOcc, &one, tmp_ppOcc, &pOcc, tmp_pOcc, &inc, &zero, tmp_pOcc2, &inc);
        // Computes cholesky of tmp_pp again stored back in tmp_ppOcc. This chol(A.beta.inv)
        F77_NAME(dpotrf)(lower, &pOcc, tmp_ppOcc, &pOcc, &info); if(info != 0){error("c++ error: dpotrf here failed\n");}
        // Args: destination, mu, cholesky of the covariance matrix, dimension
        mvrnorm(beta, tmp_pOcc2, tmp_ppOcc, pOcc);
        
        /********************************************************************
         *Update Detection Regression Coefficients
         *******************************************************************/
        // /********************************
        //  * Compute b.alpha
        //  *******************************/
        // First multiply kappDet * the current occupied values, such that values go 
        // to 0 if they z == 0 and values go to kappaDet if z == 1
        for (i = 0; i < nObs; i++) {
          // 1.0 is currently hardcoded in for occupancy data
          kappaDet[i] = (y[i] - 1.0/2.0) * z[zLongIndx[i]];
        } // i
        
        // Xp * kappaDet + 0 * tmp_pDet. Output is stored in tmp_pDet
        // dgemv computes linear combinations of different variables. 
        F77_NAME(dgemv)(ytran, &nObs, &pDet, &one, Xp, &nObs, kappaDet, &inc, &zero, tmp_pDet, &inc); 	  
        for (j = 0; j < pDet; j++) {
          tmp_pDet[j] += SigmaAlphaInvMuAlpha[j]; 
        } // j

        /********************************
         * Compute A.alpha
         * *****************************/
        for (j = 0; j < nObs; j++) {
          for (i = 0; i < pDet; i++) {
            tmp_nObspDet[i*nObs + j] = Xp[i * nObs + j] * omegaDet[j] * z[zLongIndx[j]];
          } // i
        } // j

        // This finishes off A.alpha
        // 1 * Xp * tmp_nObspDet + 0 * tmp_ppDet = tmp_ppDet
        F77_NAME(dgemm)(ytran, ntran, &pDet, &pDet, &nObs, &one, Xp, &nObs, tmp_nObspDet, &nObs, &zero, tmp_ppDet, &pDet);

        for (j = 0; j < ppDet; j++) {
          tmp_ppDet[j] += SigmaAlphaInv[j]; 
        } // j

        // This gives the Cholesky of A.alpha
        // Computes cholesky of tmp_ppDet. Output stored in tmp_ppOcc
        F77_NAME(dpotrf)(lower, &pDet, tmp_ppDet, &pDet, &info); 
        if(info != 0){error("c++ error: dpotrf A.alpha failed\n");}
        // Computes the inverse tmp_ppOcc. Stored in tmp_ppOcc. This is A.beta.inv. 
        F77_NAME(dpotri)(lower, &pDet, tmp_ppDet, &pDet, &info); 
        if(info != 0){error("c++ error: dpotri A.alpha failed\n");}
        // A.alpha.inv %*% b.alpha
        // 1 * tmp_ppDet * tmp_pDet + 0 * tmp_pDet2 
        // (which is currently nothing) = tmp_pDet2
        F77_NAME(dsymv)(lower, &pDet, &one, tmp_ppDet, &pDet, tmp_pDet, &inc, &zero, tmp_pDet2, &inc);
        // Computes cholesky of tmp_ppDet again stored back in tmp_ppDet. This chol(A.alpha.inv)
        F77_NAME(dpotrf)(lower, &pDet, tmp_ppDet, &pDet, &info); 
        if(info != 0){error("c++ error: dpotrf here failed\n");}
        // Args: destination, mu, cholesky of the covariance matrix, dimension
        mvrnorm(alpha, tmp_pDet2, tmp_ppDet, pDet);

	/********************************************************************
         *Update sigmaSq
         *******************************************************************/
	// Get inverse correlation matrix in reverse from inverse covariance matrix
	// Remember: C currently contains the inverse of covariance matrix. 
	fillUTri(C, J); 
	for (j = 0; j < JJ; j++) {
          R[j] = theta[sigmaSqIndx] * C[j]; 
	} // j
	// Compute t(w) %*% R^-1 %*% w / 
	// t(w) %*% R^-1
	// Probably a better way to do this operation. 
	for (j = 0; j < J; j++) {
          wTRInv[j] = F77_NAME(ddot)(&J, &R[j], &J, w, &inc);  
	  // Rprintf("wTRInv[j]: %f\n", wTRInv[j]); 
        } // j
	// // // wTRInv %*% w
	bSigmaSqPost = F77_NAME(ddot)(&J, wTRInv, &inc, w, &inc); 
	bSigmaSqPost /= 2.0; 
	bSigmaSqPost += sigmaSqB; 
	// Rprintf("bSigmaSqPost: %f\n", bSigmaSqPost); 
	theta[sigmaSqIndx] = rigamma(aSigmaSqPost, bSigmaSqPost); 

        /********************************************************************
         *Update phi (and nu if matern)
         *******************************************************************/
	if (corName == "matern") {
          nu = theta[nuIndx]; 
	  nuCand = logitInv(rnorm(logit(theta[nuIndx], nuA, nuB), exp(tuning[nuIndx])), nuA, nuB); 
        }
	phi = theta[phiIndx]; 
	phiCand = logitInv(rnorm(logit(phi, phiA, phiB), exp(tuning[phiIndx])), phiA, phiB); 
	theta[phiIndx] = phiCand; 
	theta[nuIndx] = nuCand; 

	// Construct covariance matrix (stored in C). 
	spCovLT(coordsD, J, theta, corName, CCand); 

        /********************************
         * Proposal
         *******************************/
	// Invert CCand and log det cov. 
        detCand = 0.0;
	F77_NAME(dpotrf)(lower, &J, CCand, &J, &info); 
	if(info != 0){error("c++ error: Cholesky failed in covariance matrix\n");}
	// Get log of the determinant of the covariance matrix. 
	for (k = 0; k < J; k++) {
	  detCand += 2.0 * log(CCand[k*J+k]);
	} // k
	F77_NAME(dpotri)(lower, &J, CCand, &J, &info); 
	if(info != 0){error("c++ error: Cholesky inverse failed in covariance matrix\n");}
        logPostCand = 0.0; 
	// Jacobian and Uniform prior. 
	logPostCand += log(phiCand - phiA) + log(phiB - phiCand); 
	// (-1/2) * tmp_JD` *  C^-1 * tmp_JD
	F77_NAME(dsymv)(lower, &J, &one,  CCand, &J, w, &inc, &zero, tmp_JD, &inc);
	logPostCand += -0.5*detCand-0.5*F77_NAME(ddot)(&J, w, &inc, tmp_JD, &inc);
        if (corName == "matern"){
          logPostCand += log(nuCand - nuA) + log(nuB - nuCand); 
        }

        /********************************
         * Current
         *******************************/
	theta[nuIndx] = nu; 
	theta[phiIndx] = phi; 
	// Construct covariance matrix (stored in C). 
	spCovLT(coordsD, J, theta, corName, C); 
        detCurr = 0.0;
	F77_NAME(dpotrf)(lower, &J, C, &J, &info); 
	if(info != 0){error("c++ error: Cholesky failed in covariance matrix\n");}
	for (k = 0; k < J; k++) {
	  detCurr += 2.0 * log(C[k*J+k]);
	} // k
	F77_NAME(dpotri)(lower, &J, C, &J, &info); 
	if(info != 0){error("c++ error: Cholesky inverse failed in covariance matrix\n");}
        logPostCurr = 0.0; 
	logPostCurr += log(phi - phiA) + log(phiB - phi); 
	// (-1/2) * tmp_JD` *  C^-1 * tmp_JD
	F77_NAME(dsymv)(lower, &J, &one, C, &J, w, &inc, &zero, tmp_JD, &inc);
	logPostCurr += -0.5*detCurr-0.5*F77_NAME(ddot)(&J, w, &inc, tmp_JD, &inc);
        if (corName == "matern"){
          logPostCurr += log(nu - nuA) + log(nuB - nu); 
        }

	// MH Accept/Reject
	logMHRatio = logPostCand - logPostCurr; 
	if (runif(0.0, 1.0) <= exp(logMHRatio)) {
          theta[phiIndx] = phiCand;
          accept[phiIndx]++;
          if (corName == "matern") {
            theta[nuIndx] = nuCand; 
            accept[nuIndx]++; 
          }
	  F77_NAME(dcopy)(&JJ, CCand, &inc, C, &inc); 
        }
	
        /********************************************************************
         *Update w (spatial random effects)
         *******************************************************************/
        /********************************
         * Compute b.w
         *******************************/
        for(j = 0; j < J; j++){
          tmp_JD[j] = kappaOcc[j] - F77_NAME(ddot)(&pOcc, &X[j], &J, beta, &inc) * omegaOcc[j];
        }
        /********************************
         * Compute A.w
         *******************************/
	// Copy inverse covariance matrix into tmp_JJ
	F77_NAME(dcopy)(&JJ, C, &inc, tmp_JJ, &inc); 
	for (k = 0; k < J; k++) {
	  tmp_JJ[k * J + k] += omegaOcc[k]; 
	} // k

        // Cholesky of A.w
        F77_NAME(dpotrf)(lower, &J, tmp_JJ, &J, &info); 
        if(info != 0){error("c++ error: dpotrf on A.w failed\n");}
	// Inverse of A.w
        F77_NAME(dpotri)(lower, &J, tmp_JJ, &J, &info); 
        if(info != 0){error("c++ error: dpotri on A.w failed\n");}
        // A.w.inv %*% b.w. Stored in tmp_JD2
        F77_NAME(dsymv)(lower, &J, &one, tmp_JJ, &J, tmp_JD, &inc, &zero, tmp_JD2, &inc);
        // Computes cholesky of tmp_JJ again stored back in tmp_JJ. This chol(A.beta.inv)
        F77_NAME(dpotrf)(lower, &J, tmp_JJ, &J, &info); 
	if(info != 0){error("c++ error: dpotrf on A.w failed\n");}
        // Args: destination, mu, cholesky of the covariance matrix, dimension
        mvrnorm(w, tmp_JD2, tmp_JJ, J);

	// At end of each MCMC, the variable C contains the inverse of the 
	// current covariance matrix. 

        /********************************************************************
         *Update Latent Occupancy
         *******************************************************************/
        // Compute detection probability 
        for (i = 0; i < nObs; i++) {
          detProb[i] = logitInv(F77_NAME(ddot)(&pDet, &Xp[i], &nObs, alpha, &inc), zero, one);
          if (tmp_J[zLongIndx[i]] == 0) {
            psi[zLongIndx[i]] = logitInv(F77_NAME(ddot)(&pOcc, &X[zLongIndx[i]], &J, beta, &inc) + w[zLongIndx[i]], zero, one); 
          }
          piProd[zLongIndx[i]] *= (1.0 - detProb[i]);
          ySum[zLongIndx[i]] += y[i]; 	
          tmp_J[zLongIndx[i]]++;
        } // i
        // Compute occupancy probability 
        for (j = 0; j < J; j++) {
          psiNum = psi[j] * piProd[j]; 
          if (ySum[j] == zero) {
            z[j] = rbinom(one, psiNum / (psiNum + (1.0 - psi[j])));           
          } else {
            z[j] = one; 
          }
          // Save z samples along the way. 
          REAL(zSamples_r)[q * J + j] = z[j]; 
          piProd[j] = one;
          ySum[j] = zero; 
          tmp_J[j] = 0; 
        } // j

        /********************************************************************
         *Replicate data set for GoF
         *******************************************************************/
        for (i = 0; i < nObs; i++) {
          yRep[i] = rbinom(one, detProb[i] * z[zLongIndx[i]]);
          INTEGER(yRepSamples_r)[q * nObs + i] = yRep[i]; 
        } // i


        /********************************************************************
         *Save samples
         *******************************************************************/
        F77_NAME(dcopy)(&pOcc, beta, &inc, &REAL(betaSamples_r)[q*pOcc], &inc);
        F77_NAME(dcopy)(&pDet, alpha, &inc, &REAL(alphaSamples_r)[q*pDet], &inc);
        F77_NAME(dcopy)(&J, psi, &inc, &REAL(psiSamples_r)[q*J], &inc); 
        F77_NAME(dcopy)(&J, w, &inc, &REAL(wSamples_r)[q*J], &inc); 
	F77_NAME(dcopy)(&nTheta, theta, &inc, &REAL(thetaSamples_r)[q*nTheta], &inc); 

        // This allows for users to interrupt the C code when running, 
        // which is not normally allowed. 
        R_CheckUserInterrupt();
      } // r (end batch)


      /********************************************************************
       *Adjust tuning 
       *******************************************************************/
      for (j = 0; j < nTheta; j++) {
        REAL(acceptSamples_r)[s * nTheta + j] = accept[j]/batchLength; 
        REAL(tuningSamples_r)[s * nTheta + j] = tuning[j]; 
        if (accept[j] / batchLength > acceptRate) {
          tuning[j] += std::min(0.01, 1.0/sqrt(static_cast<double>(s)));
        } else{
            tuning[j] -= std::min(0.01, 1.0/sqrt(static_cast<double>(s)));
          }
        accept[j] = 0;
      }
      /********************************************************************
       *Report 
       *******************************************************************/
      if (verbose) {
	if (status == nReport) {
	  Rprintf("Batch: %i of %i, %3.2f%%\n", s, nBatch, 100.0*s/nBatch);
	  Rprintf("\tparameter\tacceptance\ttuning\n");	  
	  Rprintf("\tphi\t\t%3.1f\t\t%1.5f\n", 100.0*REAL(acceptSamples_r)[s * nTheta + phiIndx], exp(tuning[phiIndx]));
	  if (corName == "matern") {
	  Rprintf("\tnu\t\t%3.1f\t\t%1.5f\n", 100.0*REAL(acceptSamples_r)[s * nTheta + nuIndx], exp(tuning[nuIndx]));
          }
	  Rprintf("-------------------------------------------------\n");
          #ifdef Win32
	  R_FlushConsole();
          #endif
	  status = 0;
	}
      }
      status++;        
    } // s (sample loop)


    // This is necessary when generating random numbers in C.     
    PutRNGstate();

    //make return object (which is a list)
    SEXP result_r, resultName_r;
    int nResultListObjs = 9;

    PROTECT(result_r = allocVector(VECSXP, nResultListObjs)); nProtect++;
    PROTECT(resultName_r = allocVector(VECSXP, nResultListObjs)); nProtect++;

    // Setting the components of the output list.
    SET_VECTOR_ELT(result_r, 0, betaSamples_r);
    SET_VECTOR_ELT(result_r, 1, alphaSamples_r);
    SET_VECTOR_ELT(result_r, 2, zSamples_r); 
    SET_VECTOR_ELT(result_r, 3, psiSamples_r);
    SET_VECTOR_ELT(result_r, 4, yRepSamples_r);
    SET_VECTOR_ELT(result_r, 5, thetaSamples_r); 
    SET_VECTOR_ELT(result_r, 6, wSamples_r); 
    SET_VECTOR_ELT(result_r, 7, tuningSamples_r); 
    SET_VECTOR_ELT(result_r, 8, acceptSamples_r); 
    // mkChar turns a C string into a CHARSXP
    SET_VECTOR_ELT(resultName_r, 0, mkChar("beta.samples")); 
    SET_VECTOR_ELT(resultName_r, 1, mkChar("alpha.samples")); 
    SET_VECTOR_ELT(resultName_r, 2, mkChar("z.samples")); 
    SET_VECTOR_ELT(resultName_r, 3, mkChar("psi.samples"));
    SET_VECTOR_ELT(resultName_r, 4, mkChar("y.rep.samples")); 
    SET_VECTOR_ELT(resultName_r, 5, mkChar("theta.samples")); 
    SET_VECTOR_ELT(resultName_r, 6, mkChar("w.samples")); 
    SET_VECTOR_ELT(resultName_r, 7, mkChar("phi.tune")); 
    SET_VECTOR_ELT(resultName_r, 8, mkChar("phi.accept")); 
   
    // Set the names of the output list.  
    namesgets(result_r, resultName_r);
    
    //unprotect
    UNPROTECT(nProtect);
    
    return(result_r);
  }
}
