#ifndef __CUFLU_DATARECONSTRUCTION__
#define __CUFLU_DATARECONSTRUCTION__



#include "CUFLU.h"

#if (  MODEL == HYDRO  &&  ( FLU_SCHEME == MHM || FLU_SCHEME == MHM_RP || FLU_SCHEME == CTU )  )



// external functions
#ifdef __CUDACC__

#include "CUFLU_Shared_FluUtility.cu"

#else

void Hydro_Rotate3D( real InOut[], const int XYZ, const bool Forward, const int Mag_Offset );
real Hydro_CheckMinPres( const real InPres, const real MinPres );
void Hydro_Con2Pri( const real In[], real Out[], const real Gamma_m1, const real MinPres,
                    const bool NormPassive, const int NNorm, const int NormIdx[],
                    const bool JeansMinPres, const real JeansMinPres_Coeff );
void Hydro_Pri2Con( const real In[], real Out[], const real _Gamma_m1,
                    const bool NormPassive, const int NNorm, const int NormIdx[] );
#if ( FLU_SCHEME == MHM )
void Hydro_Con2Flux( const int XYZ, real Flux[], const real In[], const real Gamma_m1, const real MinPres );
#endif
#ifdef MHD
void MHD_GetCellCenteredB( real B_CC[], const real Bx_FC[], const real By_FC[], const real Bz_FC[],
                           const int Width_FC, const int i, const int j, const int k );
#endif

#endif // #ifdef __CUDACC__ ... else ...


// internal functions (GPU_DEVICE is defined in CUFLU.h)
GPU_DEVICE
static void Hydro_LimitSlope( const real L[], const real C[], const real R[], const LR_Limiter_t LR_Limiter,
                              const real MinMod_Coeff, const real Gamma, const int XYZ,
                              const real LEigenVec[][NWAVE], const real REigenVec[][NWAVE],
                              real Slope_Limiter[] );
#if (  FLU_SCHEME == CTU  ||  ( defined MHD && defined CHAR_RECONSTRUCTION )  )
#ifdef MHD
static void   MHD_GetEigenSystem( const real CC_Var[], real EigenVal[],
                                  real LEigenVec[][NWAVE], real REigenVec[][NWAVE],
                                  const real Gamma, const int XYZ );
#else
static void Hydro_GetEigenSystem( const real CC_Var[], real EigenVal[][NWAVE],
                                  real LEigenVec[][NWAVE], real REigenVec[][NWAVE],
                                  const real Gamma );
#endif
#endif
#if ( FLU_SCHEME == MHM )
GPU_DEVICE
static void Hydro_HancockPredict( real fc[][NCOMP_TOTAL], const real dt, const real dh,
                                  const real Gamma_m1, const real _Gamma_m1,
                                  const real g_cc_array[][ CUBE(FLU_NXT) ], const int cc_idx,
                                  const real MinDens, const real MinPres );
#endif
#ifdef CHAR_RECONSTRUCTION
GPU_DEVICE
static void Hydro_Pri2Char( real InOut[], const real Gamma, const real Rho, const real Pres,
                            const real LEigenVec[][NWAVE], const int XYZ );
GPU_DEVICE
static void Hydro_Char2Pri( real InOut[], const real Gamma, const real Rho, const real Pres,
                            const real REigenVec[][NWAVE], const int XYZ );
#endif


// macro for adding MHD source terms in CTU
#if ( defined MHD  &&  FLU_SCHEME == CTU )
#  define MINMOD( a , b )  (  ( (a)*(b)>(real)0.0 ) ? ( SIGN(a)*FMIN(FABS(a),FABS(b)) ) : (real)0.0  )
#endif




#if ( LR_SCHEME == PLM )
//-------------------------------------------------------------------------------------------------------
// Function    :  Hydro_DataReconstruction
// Description :  Reconstruct the face-centered variables by the piecewise-linear method (PLM)
//
// Note        :  1. Use the parameter "LR_Limiter" to choose different slope limiters
//                2. Input data can be either conserved or primitive variables
//                   --> If the input data are conserved variables, one must provide g_ConVar[] and enable "Con2Pri"
//                       --> Primitive variables will be calculated by this function and stored in g_PriVar[]
//                       --> g_PriVar[] must be allocated in advance but it doesn't need to be initialized
//                       --> Adopted by MHM and CTU
//                   --> If the input data are primitive variables, one must provide g_PriVar[] and disable "Con2Pri"
//                       --> g_ConVar[] is useless here
//                           --> Although Hydro_HancockPredict() still needs g_ConVar(), MHM provides conserved instead
//                               of primitive variables. So it's fine.
//                       --> Adopted by MHM_RP, where Hydro_RiemannPredict() already returns primitive variables
//                3. Output data are always conserved variables
//                   --> Because Hydro_HancockPredict() only works with conserved variables
//                4. PLM and PPM data reconstruction functions share the same function name
//                5. Face-centered variables will be advanced by half time-step for the MHM and CTU schemes
//                6. Data reconstruction can be applied to characteristic variables by
//                   defining "CHAR_RECONSTRUCTION" in the header CUFLU.h
//                7. This function is shared by MHM, MHM_RP, and CTU schemes
//
// Parameter   :  g_ConVar           : Array storing the input cell-centered conserved variables
//                                     --> Should contain NCOMP_TOTAL variables
//                g_FC_B             : Array storing the input face-centered magnetic field (for MHD only)
//                                     --> Should contain NCOMP_MAGNETIC variables
//                g_PriVar           : Array storing/to store the cell-centered primitive variables
//                                     --> Should contain NCOMP_TOTAL_PLUS_MAG variables
//                                         --> For MHD, this array currently stores the normal B field as well
//                                     --> For MHM, g_ConVar[] and g_PriVar[] must point to different arrays since
//                                         Hydro_HancockPredict() requires the original g_ConVar[]
//                g_FC_Var           : Array to store the output face-centered primitive variables
//                                     --> Should contain NCOMP_TOTAL_PLUS_MAG variables
//                g_Slope_PPM        : Array to store the x/y/z slopes for the PPM reconstruction
//                                     --> Should contain NCOMP_TOTAL_PLUS_MAG variables
//                Con2Pri            : Convert conserved variables in g_ConVar[] to primitive variables and
//                                     store the results in g_PriVar[]
//                NIn                : Size of g_PriVar[] along each direction
//                                     --> Can be smaller than FLU_NXT
//                NGhost             : Number of ghost zones
//                                      --> "NIn-2*NGhost" cells will be computed along each direction
//                                      --> Size of g_FC_Var[] is assumed to be "(NIn-2*NGhost)^3"
//                                      --> The reconstructed data at cell (i,j,k) will be stored in g_FC_Var[]
//                                          with the index "(i-NGhost,j-NGhost,k-NGhost)"
//                Gamma              : Ratio of specific heats
//                LR_Limiter         : Slope limiter for the data reconstruction in the MHM/MHM_RP/CTU schemes
//                                     (0/1/2/3) = (vanLeer/generalized MinMod/vanAlbada/vanLeer+generalized MinMod) limiter
//                MinMod_Coeff       : Coefficient of the generalized MinMod limiter
//                dt                 : Time interval to advance solution (for the CTU scheme)
//                dh                 : Cell size
//                MinDens/Pres       : Minimum allowed density and pressure
//                NormPassive        : true --> convert passive scalars to mass fraction
//                NNorm              : Number of passive scalars for the option "NormPassive"
//                                     --> Should be set to the global variable "PassiveNorm_NVar"
//                NormIdx            : Target variable indices for the option "NormPassive"
//                                     --> Should be set to the global variable "PassiveNorm_VarIdx"
//                JeansMinPres       : Apply minimum pressure estimated from the Jeans length
//                JeansMinPres_Coeff : Coefficient used by JeansMinPres = G*(Jeans_NCell*Jeans_dh)^2/(Gamma*pi);
//------------------------------------------------------------------------------------------------------
GPU_DEVICE
void Hydro_DataReconstruction( const real g_ConVar   [][ CUBE(FLU_NXT) ],
                               const real g_FC_B     [][ SQR(FLU_NXT)*FLU_NXT_P1 ],
                                     real g_PriVar   [][ CUBE(FLU_NXT) ],
                                     real g_FC_Var   [][NCOMP_TOTAL_PLUS_MAG][ CUBE(N_FC_VAR) ],
                                     real g_Slope_PPM[][NCOMP_TOTAL_PLUS_MAG][ CUBE(N_SLOPE_PPM) ],
                               const bool Con2Pri, const int NIn, const int NGhost, const real Gamma,
                               const LR_Limiter_t LR_Limiter, const real MinMod_Coeff,
                               const real dt, const real dh, const real MinDens, const real MinPres,
                               const bool NormPassive, const int NNorm, const int NormIdx[],
                               const bool JeansMinPres, const real JeansMinPres_Coeff )
{

// check
#  ifdef GAMER_DEBUG
   if ( NIn - 2*NGhost != N_FC_VAR )
      printf( "ERROR : NIn - 2*NGhost != N_FC_VAR (NIn %d, NGhost %d, N_FC_VAR %d) !!\n",
              NIn, NGhost, N_FC_VAR );
#  endif


   const int  didx_cc[3] = { 1, NIn, SQR(NIn) };
   const int  NOut       = NIn - 2*NGhost;      // number of output cells
   const real  Gamma_m1  = Gamma - (real)1.0;
   const real _Gamma_m1  = (real)1.0 / Gamma_m1;

#  if ( FLU_SCHEME == CTU )
   const real dt_dh2 = (real)0.5*dt/dh;

// index mapping between arrays with size NWAVE and NCOMP_TOTAL_PLUS_MAG;
#  ifdef MHD
   const int idx_wave[NWAVE] = { 0, 1, 2, 3, 4, MAG_OFFSET+1, MAG_OFFSET+2 };
#  else
   const int idx_wave[NWAVE] = { 0, 1, 2, 3, 4 };
#  endif
#  endif // #if ( FLU_SCHEME == CTU )

// eigenvalues and eigenvectors
// --> constant components of the left and right eigenvector matrices must be initialized
#  if ( FLU_SCHEME == CTU )
   real EigenVal[3][NWAVE];
#  ifdef MHD
   real REigenVec[NWAVE][NWAVE] = { { NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL },
                                    {       0.0,       0.0, NULL_REAL, NULL_REAL,       0.0, NULL_REAL, NULL_REAL },
                                    { NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL },
                                    {       1.0,       0.0,       0.0,       0.0,       0.0,       0.0,       0.0 },
                                    { NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL },
                                    {       0.0,       0.0, NULL_REAL, NULL_REAL,       0.0, NULL_REAL, NULL_REAL },
                                    { NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL } };
   real LEigenVec[NWAVE][NWAVE] = { { 0.0, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL },
                                    { 0.0,       0.0, NULL_REAL, NULL_REAL,       0.0, NULL_REAL, NULL_REAL },
                                    { 0.0, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL },
                                    { 1.0,       0.0,       0.0,       0.0, NULL_REAL,       0.0,       0.0 },
                                    { 0.0, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL },
                                    { 0.0,       0.0, NULL_REAL, NULL_REAL,       0.0, NULL_REAL, NULL_REAL },
                                    { 0.0, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL } };

#  else
   real LEigenVec[NWAVE][NWAVE] = { { 0.0, NULL_REAL, 0.0, 0.0, NULL_REAL },
                                    { 1.0,       0.0, 0.0, 0.0, NULL_REAL },
                                    { 0.0,       0.0, 1.0, 0.0,       0.0 },
                                    { 0.0,       0.0, 0.0, 1.0,       0.0 },
                                    { 0.0, NULL_REAL, 0.0, 0.0, NULL_REAL } };

   real REigenVec[NWAVE][NWAVE] = { { 1.0, NULL_REAL, 0.0, 0.0, NULL_REAL },
                                    { 1.0,       0.0, 0.0, 0.0,       0.0 },
                                    { 0.0,       0.0, 1.0, 0.0,       0.0 },
                                    { 0.0,       0.0, 0.0, 1.0,       0.0 },
                                    { 1.0, NULL_REAL, 0.0, 0.0, NULL_REAL } };
#  endif // #ifdef MHD ... else ...

#  elif ( defined MHD  &&  defined CHAR_RECONSTRUCTION ) // #if ( FLU_SCHEME == CTU )
   real EigenVal[3][NWAVE];
   real REigenVec[NWAVE][NWAVE] = { { NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL },
                                    {       0.0,       0.0, NULL_REAL, NULL_REAL,       0.0, NULL_REAL, NULL_REAL },
                                    { NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL },
                                    {       1.0,       0.0,       0.0,       0.0,       0.0,       0.0,       0.0 },
                                    { NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL },
                                    {       0.0,       0.0, NULL_REAL, NULL_REAL,       0.0, NULL_REAL, NULL_REAL },
                                    { NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL } };
   real LEigenVec[NWAVE][NWAVE] = { { 0.0, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL },
                                    { 0.0,       0.0, NULL_REAL, NULL_REAL,       0.0, NULL_REAL, NULL_REAL },
                                    { 0.0, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL },
                                    { 1.0,       0.0,       0.0,       0.0, NULL_REAL,       0.0,       0.0 },
                                    { 0.0, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL },
                                    { 0.0,       0.0, NULL_REAL, NULL_REAL,       0.0, NULL_REAL, NULL_REAL },
                                    { 0.0, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL, NULL_REAL } };

#  else // #if ( FLU_SCHEME == CTU ) ... elif ...
   real (*const REigenVec)[NWAVE] = NULL;
   real (*const LEigenVec)[NWAVE] = NULL;
#  endif // #if ( FLU_SCHEME ==  CTU ) ... elif ... else ...


// 0. conserved --> primitive variables
   if ( Con2Pri )
   {
      real ConVar_1Cell[NCOMP_TOTAL_PLUS_MAG], PriVar_1Cell[NCOMP_TOTAL_PLUS_MAG];

      CGPU_LOOP( idx, CUBE(NIn) )
      {
         for (int v=0; v<NCOMP_TOTAL; v++)   ConVar_1Cell[v] = g_ConVar[v][idx];

#        ifdef MHD
//       assuming that g_FC_B[] has the size of NIn*NIn*(NIn+1)
         const int size_ij = SQR( NIn );
         const int i       = idx % NIn;
         const int j       = idx % size_ij / NIn;
         const int k       = idx / size_ij;

         MHD_GetCellCenteredB( ConVar_1Cell+NCOMP_TOTAL, g_FC_B[0], g_FC_B[1], g_FC_B[2], NIn, i, j, k );
#        endif

         Hydro_Con2Pri( ConVar_1Cell, PriVar_1Cell, Gamma_m1, MinPres, NormPassive, NNorm, NormIdx,
                        JeansMinPres, JeansMinPres_Coeff );

         for (int v=0; v<NCOMP_TOTAL_PLUS_MAG; v++)   g_PriVar[v][idx] = PriVar_1Cell[v];
      }

#     ifdef __CUDACC__
      __syncthreads();
#     endif
   } // if ( Con2Pri )


// data reconstruction
   const int NOut2  = SQR(NOut);
#  ifdef MHD
   const int NIn_p1 = NIn + 1;
   int idx_B[NCOMP_MAGNETIC];
#  endif

   CGPU_LOOP( idx_fc, CUBE(NOut) )
   {
      const int i_cc   = NGhost + idx_fc%NOut;
      const int j_cc   = NGhost + idx_fc%NOut2/NOut;
      const int k_cc   = NGhost + idx_fc/NOut2;
      const int idx_cc = IDX321( i_cc, j_cc, k_cc, NIn, NIn );

#     ifdef MHD
//    assuming that g_FC_B[] has the size of NIn*NIn*(NIn+1)
      idx_B[0] = (k_cc*NIn    + i_cc)*NIn_p1 + i_cc;
      idx_B[1] = (k_cc*NIn_p1 + i_cc)*NIn    + i_cc;
      idx_B[2] = (k_cc*NIn    + i_cc)*NIn    + i_cc;
#     endif

//    cc_C/L/R: cell-centered variables of the Central/Left/Right cells
//    fc: face-centered variables of the central cell
      real cc_C[NCOMP_TOTAL_PLUS_MAG], cc_L[NCOMP_TOTAL_PLUS_MAG], cc_R[NCOMP_TOTAL_PLUS_MAG];
      real fc[6][NCOMP_TOTAL_PLUS_MAG], Slope_Limiter[NCOMP_TOTAL_PLUS_MAG];

      for (int v=0; v<NCOMP_TOTAL_PLUS_MAG; v++)   cc_C[v] = g_PriVar[v][idx_cc];


//    1-a. evaluate the eigenvalues and eigenvectors along all three directions for the pure-hydro CTU integrator
#     if ( !defined MHD  &&  FLU_SCHEME == CTU )
      Hydro_GetEigenSystem( cc_C, EigenVal, LEigenVec, REigenVec, Gamma );
#     endif


//    loop over different spatial directions
      for (int d=0; d<3; d++)
      {
//       1-b. evaluate the eigenvalues and eigenvectors along the target direction for the MHD CTU integrator
#        if (  defined MHD  &&  ( FLU_SCHEME == CTU || defined CHAR_RECONSTRUCTION )  )
         MHD_GetEigenSystem( cc_C, EigenVal[d], LEigenVec, REigenVec, Gamma, d );
#        endif


//       2. evaluate the monotonic slope
         const int faceL   = 2*d;      // left and right face indices
         const int faceR   = faceL+1;
         const int idx_ccL = idx_cc - didx_cc[d];
         const int idx_ccR = idx_cc + didx_cc[d];

         for (int v=0; v<NCOMP_TOTAL_PLUS_MAG; v++)
         {
            cc_L[v] = g_PriVar[v][idx_ccL];
            cc_R[v] = g_PriVar[v][idx_ccR];
         }

         Hydro_LimitSlope( cc_L, cc_C, cc_R, LR_Limiter, MinMod_Coeff, Gamma, d,
                           LEigenVec, REigenVec, Slope_Limiter );


//       3. get the face-centered primitive variables
         for (int v=0; v<NCOMP_TOTAL_PLUS_MAG; v++)
         {
            fc[faceL][v] = cc_C[v] - (real)0.5*Slope_Limiter[v];
            fc[faceR][v] = cc_C[v] + (real)0.5*Slope_Limiter[v];
         }

//       ensure the face-centered variables lie between neighboring cell-centered values
         for (int v=0; v<NCOMP_TOTAL_PLUS_MAG; v++)
         {
            real Min, Max;

            Min = ( cc_C[v] < cc_L[v] ) ? cc_C[v] : cc_L[v];
            Max = ( cc_C[v] > cc_L[v] ) ? cc_C[v] : cc_L[v];
            fc[faceL][v] = ( fc[faceL][v] > Min ) ? fc[faceL][v] : Min;
            fc[faceL][v] = ( fc[faceL][v] < Max ) ? fc[faceL][v] : Max;
            fc[faceR][v] = (real)2.0*cc_C[v] - fc[faceL][v];

            Min = ( cc_C[v] < cc_R[v] ) ? cc_C[v] : cc_R[v];
            Max = ( cc_C[v] > cc_R[v] ) ? cc_C[v] : cc_R[v];
            fc[faceR][v] = ( fc[faceR][v] > Min ) ? fc[faceR][v] : Min;
            fc[faceR][v] = ( fc[faceR][v] < Max ) ? fc[faceR][v] : Max;
            fc[faceL][v] = (real)2.0*cc_C[v] - fc[faceR][v];
         }


//       4. advance the face-centered variables by half time-step for the CTU integrator
#        if ( FLU_SCHEME == CTU )
         real Coeff_L, Coeff_R;
         real Correct_L[NCOMP_TOTAL_PLUS_MAG], Correct_R[NCOMP_TOTAL_PLUS_MAG], dfc[NCOMP_TOTAL_PLUS_MAG];

//       4-1. evaluate the slope (for passive scalars as well)
         for (int v=0; v<NCOMP_TOTAL_PLUS_MAG; v++)   dfc[v] = fc[faceR][v] - fc[faceL][v];


//       4-2. re-order variables for the y/z directions
         Hydro_Rotate3D( dfc, d, true, MAG_OFFSET );


//       =====================================================================================
//       a. for the HLL solvers (HLLE/HLLC)
//       =====================================================================================
#        if (  ( RSOLVER == HLLE || RSOLVER == HLLC )  &&  defined HLL_NO_REF_STATE  )

//       4-2-a1. evaluate the corrections to the left and right face-centered variables

         for (int v=0; v<NWAVE; v++)
         {
            Correct_L[ idx_wave[v] ] = (real)0.0;
            Correct_R[ idx_wave[v] ] = (real)0.0;
         }

#        ifdef HLL_INCLUDE_ALL_WAVES

         for (int Mode=0; Mode<NWAVE; Mode++)
         {
            Coeff_L = (real)0.0;

            for (int v=0; v<NWAVE; v++)   Coeff_L += LEigenVec[Mode][v]*dfc[ idx_wave[v] ];

            Coeff_L *= -dt_dh2*EigenVal[d][Mode];

            for (int v=0; v<NWAVE; v++)   Correct_L[ idx_wave[v] ] += Coeff_L*REigenVec[Mode][v];
         } // for (int Mode=0; Mode<NWAVE; Mode++)

         for (int v=0; v<NWAVE; v++)   Correct_R[ idx_wave[v] ] = Correct_L[ idx_wave[v] ];

#        else // #ifdef HLL_INCLUDE_ALL_WAVES

         for (int Mode=0; Mode<NWAVE; Mode++)
         {
            Coeff_L = (real)0.0;
            Coeff_R = (real)0.0;

            if ( EigenVal[d][Mode] <= (real)0.0 )
            {
               for (int v=0; v<NWAVE; v++)   Coeff_L += LEigenVec[Mode][v]*dfc[ idx_wave[v] ];

               Coeff_L *= -dt_dh2*EigenVal[d][Mode];

               for (int v=0; v<NWAVE; v++)   Correct_L[ idx_wave[v] ] += Coeff_L*REigenVec[Mode][v];
            }

            if ( EigenVal[d][Mode] >= (real)0.0 )
            {
               for (int v=0; v<NWAVE; v++)   Coeff_R += LEigenVec[Mode][v]*dfc[ idx_wave[v] ];

               Coeff_R *= -dt_dh2*EigenVal[d][Mode];

               for (int v=0; v<NWAVE; v++)   Correct_R[ idx_wave[v] ] += Coeff_R*REigenVec[Mode][v];
            }
         } // for (int Mode=0; Mode<NWAVE; Mode++)

#        endif // #ifdef HLL_INCLUDE_ALL_WAVES ... else ...


//       =====================================================================================
//       b. for the Roe's and exact solvers
//       =====================================================================================
#        else // ( RSOLVER == ROE/EXACT || ifndef HLL_NO_REF_STATE )

//       4-2-b1. evaluate the reference states
         Coeff_L = -dt_dh2*FMIN( EigenVal[d][       0 ], (real)0.0 );
         Coeff_R = -dt_dh2*FMAX( EigenVal[d][ NWAVE-1 ], (real)0.0 );

         for (int v=0; v<NWAVE; v++)
         {
            Correct_L[ idx_wave[v] ] = Coeff_L*dfc[ idx_wave[v] ];
            Correct_R[ idx_wave[v] ] = Coeff_R*dfc[ idx_wave[v] ];
         }


//       4-2-b2. evaluate the corrections to the left and right face-centered variables
         for (int Mode=0; Mode<NWAVE; Mode++)
         {
            Coeff_L = (real)0.0;
            Coeff_R = (real)0.0;

            if ( EigenVal[d][Mode] <= (real)0.0 )
            {
               for (int v=0; v<NWAVE; v++)   Coeff_L += LEigenVec[Mode][v]*dfc[ idx_wave[v] ];

               Coeff_L *= dt_dh2*( EigenVal[d][0] - EigenVal[d][Mode] );

               for (int v=0; v<NWAVE; v++)   Correct_L[ idx_wave[v] ] += Coeff_L*REigenVec[Mode][v];
            }

            if ( EigenVal[d][Mode] >= (real)0.0 )
            {
               for (int v=0; v<NWAVE; v++)   Coeff_R += LEigenVec[Mode][v]*dfc[ idx_wave[v] ];

               Coeff_R *= dt_dh2*( EigenVal[d][4] - EigenVal[d][Mode] );

               for (int v=0; v<NWAVE; v++)   Correct_R[ idx_wave[v] ] += Coeff_R*REigenVec[Mode][v];
            }
         } // for (int Mode=0; Mode<NWAVE; Mode++)

#        endif // if (  ( RSOLVER == HLLE || RSOLVER == HLLC )  &&  defined HLL_NO_REF_STATE  ) ... else ...


//       4-2-b3. evaluate the corrections to the left and right face-centered passive scalars
//               --> passive scalars travel with fluid velocity (i.e., entropy mode)
#        if ( NCOMP_PASSIVE > 0 )
         Coeff_L = -dt_dh2*FMIN( EigenVal[d][1], (real)0.0 );
         Coeff_R = -dt_dh2*FMAX( EigenVal[d][1], (real)0.0 );

         for (int v=NCOMP_FLUID; v<NCOMP_TOTAL; v++)
         {
            Correct_L[v] = Coeff_L*dfc[v];
            Correct_R[v] = Coeff_R*dfc[v];
         }
#        endif


//       4-2-b4. add the MHD source terms
#        ifdef MHD
         const int t1 = (d+1)%3;    // transverse direction 1
         const int t2 = (d+2)%3;    // transverse direction 2
         real B_nL, B_nR, B_t1L, B_t1R, B_t2L, B_t2R;
         real dB_n, dB_t1, dB_t2, v_t1, v_t2, src_t1, src_t2;

         B_nL   = g_FC_B[d ][ idx_B[d ] ];
         B_t1L  = g_FC_B[t1][ idx_B[t1] ];
         B_t2L  = g_FC_B[t2][ idx_B[t2] ];
         B_nR   = g_FC_B[d ][ idx_B[d ] + didx_cc[d ] ];
         B_t1R  = g_FC_B[t1][ idx_B[t1] + didx_cc[t1] ];
         B_t2R  = g_FC_B[t2][ idx_B[t2] + didx_cc[t2] ];

         dB_n   = B_nR  - B_nL;
         dB_t1  = B_t1R - B_t1L;
         dB_t2  = B_t2R - B_t2L;

         v_t1   = cc_C[ 1 + t1 ];
         v_t2   = cc_C[ 1 + t2 ];

         src_t1 = dt_dh2*v_t1*MINMOD( dB_n, -dB_t1 );
         src_t2 = dt_dh2*v_t2*MINMOD( dB_n, -dB_t2 );

         Correct_L[ MAG_OFFSET + 1 ] += src_t1;
         Correct_R[ MAG_OFFSET + 1 ] += src_t1;
         Correct_L[ MAG_OFFSET + 2 ] += src_t2;
         Correct_R[ MAG_OFFSET + 2 ] += src_t2;
#        endif // #ifdef MHD


//       4-2-b5. evaluate the face-centered variables at the half time-step
         Hydro_Rotate3D( Correct_L, d, false, MAG_OFFSET );
         Hydro_Rotate3D( Correct_R, d, false, MAG_OFFSET );

         for (int v=0; v<NCOMP_TOTAL_PLUS_MAG; v++)
         {
            fc[faceL][v] += Correct_L[v];
            fc[faceR][v] += Correct_R[v];
         }

//       ensure positive density and pressure
         fc[faceL][0] = FMAX( fc[faceL][0], MinDens );
         fc[faceR][0] = FMAX( fc[faceR][0], MinDens );

         fc[faceL][4] = Hydro_CheckMinPres( fc[faceL][4], MinPres );
         fc[faceR][4] = Hydro_CheckMinPres( fc[faceR][4], MinPres );

#        if ( NCOMP_PASSIVE > 0 )
         for (int v=NCOMP_FLUID; v<NCOMP_TOTAL; v++) {
         fc[faceL][v] = FMAX( fc[faceL][v], TINY_NUMBER );
         fc[faceR][v] = FMAX( fc[faceR][v], TINY_NUMBER ); }
#        endif

#        endif // #if ( FLU_SCHEME == CTU )


//       5. reset the longitudinal B field to the input face-centered values
//          --> actually no data reconstruction is required for that
//###OPTIMIZARION: do not perform data reconstruction for the longitudinal B field
#        ifdef MHD
         fc[faceL][ MAG_OFFSET + d ] = B_nL;
         fc[faceR][ MAG_OFFSET + d ] = B_nR;
#        endif


//       6. primitive variables --> conserved variables
         real tmp[NCOMP_TOTAL_PLUS_MAG];  // input and output arrays must not overlap for Pri2Con()

         for (int v=0; v<NCOMP_TOTAL_PLUS_MAG; v++)   tmp[v] = fc[faceL][v];
         Hydro_Pri2Con( tmp, fc[faceL], _Gamma_m1, NormPassive, NNorm, NormIdx );

         for (int v=0; v<NCOMP_TOTAL_PLUS_MAG; v++)   tmp[v] = fc[faceR][v];
         Hydro_Pri2Con( tmp, fc[faceR], _Gamma_m1, NormPassive, NNorm, NormIdx );

      } // for (int d=0; d<3; d++)


#     if ( FLU_SCHEME == MHM )
//    7. advance the face-centered variables by half time-step for the MHM integrator
      Hydro_HancockPredict( fc, dt, dh, Gamma_m1, _Gamma_m1, g_ConVar, idx_cc, MinDens, MinPres );
#     endif


//    8. store the face-centered values to the output array
      for (int f=0; f<6; f++)
      for (int v=0; v<NCOMP_TOTAL_PLUS_MAG; v++)
         g_FC_Var[f][v][idx_fc] = fc[f][v];

   } // CGPU_LOOP( idx_fc, CUBE(NOut) )


#  ifdef __CUDACC__
   __syncthreads();
#  endif

} // FUNCTION : Hydro_DataReconstruction (PLM)
#endif // #if ( LR_SCHEME == PLM )



#if ( LR_SCHEME == PPM )
//-------------------------------------------------------------------------------------------------------
// Function    :  Hydro_DataReconstruction
// Description :  Reconstruct the face-centered variables by the piecewise-parabolic method (PPM)
//
// Note        :  See the PLM routine
//
// Parameter   :  See the PLM routine
//------------------------------------------------------------------------------------------------------
GPU_DEVICE
void Hydro_DataReconstruction( const real g_ConVar   [][ CUBE(FLU_NXT) ],
                               const real g_FC_B     [][ SQR(FLU_NXT)*FLU_NXT_P1 ],
                                     real g_PriVar   [][ CUBE(FLU_NXT) ],
                                     real g_FC_Var   [][NCOMP_TOTAL_PLUS_MAG][ CUBE(N_FC_VAR) ],
                                     real g_Slope_PPM[][NCOMP_TOTAL_PLUS_MAG][ CUBE(N_SLOPE_PPM) ],
                               const bool Con2Pri, const int NIn, const int NGhost, const real Gamma,
                               const LR_Limiter_t LR_Limiter, const real MinMod_Coeff,
                               const real dt, const real dh, const real MinDens, const real MinPres,
                               const bool NormPassive, const int NNorm, const int NormIdx[],
                               const bool JeansMinPres, const real JeansMinPres_Coeff )
{

// check
#  ifdef GAMER_DEBUG
   if ( NIn - 2*NGhost != N_FC_VAR )
      printf( "ERROR : NIn - 2*NGhost != N_FC_VAR (NIn %d, NGhost %d, N_FC_VAR %d) !!\n",
              NIn, NGhost, N_FC_VAR );
#  endif


   const int  didx_cc   [3] = { 1, NIn, SQR(NIn) };
   const int  didx_slope[3] = { 1, N_SLOPE_PPM, SQR(N_SLOPE_PPM) };
   const int  NOut       = NIn - 2*NGhost;      // number of output cells
   const int  NSlope     = N_SLOPE_PPM;         // size of g_Slope_PPM[] (which must be equal to NOut + 2)
   const real  Gamma_m1  = Gamma - (real)1.0;
   const real _Gamma_m1  = (real)1.0 / Gamma_m1;


#  if ( FLU_SCHEME == CTU )
   const real dt_dh2 = (real)0.5*dt/dh;

// include waves both from left and right directions during the data reconstruction, as suggested in ATHENA
#  if (  ( RSOLVER == HLLE || RSOLVER == HLLC )  &&  defined HLL_NO_REF_STATE  )
#  ifdef HLL_INCLUDE_ALL_WAVES
   const bool HLL_Include_All_Waves = true;
#  else
   const bool HLL_Include_All_Waves = false;
#  endif
#  endif // if (  ( RSOLVER == HLLE || RSOLVER == HLLC )  &&  defined HLL_NO_REF_STATE  )

// constant components of the left and right eigenvector matrices must be initialized
   real LEigenVec[NCOMP_FLUID][NCOMP_FLUID] = { { 0.0, NULL_REAL, 0.0, 0.0, NULL_REAL },
                                                { 1.0,       0.0, 0.0, 0.0, NULL_REAL },
                                                { 0.0,       0.0, 1.0, 0.0,       0.0 },
                                                { 0.0,       0.0, 0.0, 1.0,       0.0 },
                                                { 0.0, NULL_REAL, 0.0, 0.0, NULL_REAL } };

   real REigenVec[NCOMP_FLUID][NCOMP_FLUID] = { { 1.0, NULL_REAL, 0.0, 0.0, NULL_REAL },
                                                { 1.0,       0.0, 0.0, 0.0,       0.0 },
                                                { 0.0,       0.0, 1.0, 0.0,       0.0 },
                                                { 0.0,       0.0, 0.0, 1.0,       0.0 },
                                                { 1.0, NULL_REAL, 0.0, 0.0, NULL_REAL } };
#  endif // #if ( FLU_SCHEME == CTU )


// 0. conserved --> primitive variables
   if ( Con2Pri )
   {
      real ConVar_1Cell[NCOMP_TOTAL], PriVar_1Cell[NCOMP_TOTAL];

      CGPU_LOOP( idx, CUBE(NIn) )
      {
         for (int v=0; v<NCOMP_TOTAL; v++)   ConVar_1Cell[v] = g_ConVar[v][idx];

         Hydro_Con2Pri( ConVar_1Cell, PriVar_1Cell, Gamma_m1, MinPres, NormPassive, NNorm, NormIdx,
                        JeansMinPres, JeansMinPres_Coeff );

         for (int v=0; v<NCOMP_TOTAL; v++)   g_PriVar[v][idx] = PriVar_1Cell[v];
      }

#     ifdef __CUDACC__
      __syncthreads();
#     endif
   } // if ( Con2Pri )


// 1. evaluate the monotonic slope of all cells
   const int NSlope2 = SQR(NSlope);
   CGPU_LOOP( idx_slope, CUBE(NSlope) )
   {
      const int i_cc   = NGhost - 1 + idx_slope%NSlope;
      const int j_cc   = NGhost - 1 + idx_slope%NSlope2/NSlope;
      const int k_cc   = NGhost - 1 + idx_slope/NSlope2;
      const int idx_cc = IDX321( i_cc, j_cc, k_cc, NIn, NIn );

      real cc_C[NCOMP_TOTAL], cc_L[NCOMP_TOTAL], cc_R[NCOMP_TOTAL];  // cell-centered variables of the Central/Left/Right cells
      real Slope_Limiter[NCOMP_TOTAL];

      for (int v=0; v<NCOMP_TOTAL; v++)   cc_C[v] = g_PriVar[v][idx_cc];

//    loop over different spatial directions
      for (int d=0; d<3; d++)
      {
         const int idx_ccL = idx_cc - didx_cc[d];
         const int idx_ccR = idx_cc + didx_cc[d];

         for (int v=0; v<NCOMP_TOTAL; v++)
         {
            cc_L[v] = g_PriVar[v][idx_ccL];
            cc_R[v] = g_PriVar[v][idx_ccR];
         }

         Hydro_LimitSlope( cc_L, cc_C, cc_R, LR_Limiter, MinMod_Coeff, Gamma, d, Slope_Limiter );

//       store the results to g_Slope_PPM[]
         for (int v=0; v<NCOMP_TOTAL; v++)   g_Slope_PPM[d][v][idx_slope] = Slope_Limiter[v];

      } // for (int d=0; d<3; d++)
   } // CGPU_LOOP( idx_slope, CUBE(NSlope) )

#  ifdef __CUDACC__
   __syncthreads();
#  endif


// data reconstruction
   const int NOut2 = SQR(NOut);
   CGPU_LOOP( idx_fc, CUBE(NOut) )
   {
      const int i_fc      = idx_fc%NOut;
      const int j_fc      = idx_fc%NOut2/NOut;
      const int k_fc      = idx_fc/NOut2;

      const int i_cc      = i_fc + NGhost;
      const int j_cc      = j_fc + NGhost;
      const int k_cc      = k_fc + NGhost;
      const int idx_cc    = IDX321( i_cc, j_cc, k_cc, NIn, NIn );

      const int i_slope   = i_fc + 1;   // because NSlope = NOut + 2
      const int j_slope   = j_fc + 1;
      const int k_slope   = k_fc + 1;
      const int idx_slope = IDX321( i_slope, j_slope, k_slope, NSlope, NSlope );

 //   cc/fc: cell/face-centered variables; _C_ncomp: central cell with all NCOMP_TOTAL variables
      real cc_C_ncomp[NCOMP_TOTAL], fc[6][NCOMP_TOTAL], dfc[NCOMP_TOTAL], dfc6[NCOMP_TOTAL];
#     if ( FLU_SCHEME == CTU )
      real EigenVal[3][NCOMP_FLUID], Correct_L[NCOMP_TOTAL], Correct_R[NCOMP_TOTAL];
      real Coeff_L, Coeff_R;
#     endif

      for (int v=0; v<NCOMP_TOTAL; v++)   cc_C_ncomp[v] = g_PriVar[v][idx_cc];


//    2. evaluate the eigenvalues and eigenvectors for the CTU integrator
#     if ( FLU_SCHEME == CTU )
      Hydro_GetEigenSystem( cc_C_ncomp, EigenVal, LEigenVec, REigenVec, Gamma );
#     endif


//    3. get the face-centered primitive variables
//    loop over different spatial directions
      for (int d=0; d<3; d++)
      {
         const int faceL      = 2*d;      // left and right face indices
         const int faceR      = faceL+1;
         const int idx_ccL    = idx_cc - didx_cc[d];
         const int idx_ccR    = idx_cc + didx_cc[d];
         const int idx_slopeL = idx_slope - didx_slope[d];
         const int idx_slopeR = idx_slope + didx_slope[d];

         for (int v=0; v<NCOMP_TOTAL; v++)
         {
//          cc/fc: cell/face-centered variables; _C/L/R: Central/Left/Right cells
            real cc_C, cc_L, cc_R, dcc_L, dcc_R, dcc_C, fc_L, fc_R, Max, Min;

//          3-1. parabolic interpolation
            cc_L  = g_PriVar[v][idx_ccL];
            cc_R  = g_PriVar[v][idx_ccR];
            cc_C  = cc_C_ncomp[v];

            dcc_L = g_Slope_PPM[d][v][idx_slopeL];
            dcc_R = g_Slope_PPM[d][v][idx_slopeR];
            dcc_C = g_Slope_PPM[d][v][idx_slope ];

            fc_L  = (real)0.5*( cc_C + cc_L ) - (real)1.0/(real)6.0*( dcc_C - dcc_L );
            fc_R  = (real)0.5*( cc_C + cc_R ) - (real)1.0/(real)6.0*( dcc_R - dcc_C );


//          3-2. monotonicity constraint
            dfc [v] = fc_R - fc_L;
            dfc6[v] = (real)6.0*(  cc_C - (real)0.5*( fc_L + fc_R )  );

            if (  ( fc_R - cc_C )*( cc_C - fc_L ) <= (real)0.0  )
            {
               fc_L = cc_C;
               fc_R = cc_C;
            }
            else if ( dfc[v]*dfc6[v] > +dfc[v]*dfc[v] )
               fc_L = (real)3.0*cc_C - (real)2.0*fc_R;
            else if ( dfc[v]*dfc6[v] < -dfc[v]*dfc[v] )
               fc_R = (real)3.0*cc_C - (real)2.0*fc_L;


//          3-3. ensure the face-centered variables lie between neighboring cell-centered values
            Min  = ( cc_C < cc_L ) ? cc_C : cc_L;
            Max  = ( cc_C > cc_L ) ? cc_C : cc_L;
            fc_L = ( fc_L > Min  ) ? fc_L : Min;
            fc_L = ( fc_L < Max  ) ? fc_L : Max;

            Min  = ( cc_C < cc_R ) ? cc_C : cc_R;
            Max  = ( cc_C > cc_R ) ? cc_C : cc_R;
            fc_R = ( fc_R > Min  ) ? fc_R : Min;
            fc_R = ( fc_R < Max  ) ? fc_R : Max;

            fc[faceL][v] = fc_L;
            fc[faceR][v] = fc_R;

         } // for (int v=0; v<NCOMP_TOTAL; v++)


//       4. advance the face-centered variables by half time-step for the CTU integrator
#        if ( FLU_SCHEME == CTU )

//       4-1. compute the PPM coefficient (for the passive scalars as well)
         for (int v=0; v<NCOMP_TOTAL; v++)
         {
            dfc [v] = fc[faceR][v] - fc[faceL][v];
            dfc6[v] = (real)6.0*(  cc_C_ncomp[v] - (real)0.5*( fc[faceL][v] + fc[faceR][v] )  );
         }

//       4-2. re-order variables for the y/z directions
         Hydro_Rotate3D( dfc,  d, true );
         Hydro_Rotate3D( dfc6, d, true );


//       =====================================================================================
//       a. for the HLL solvers (HLLE/HLLC)
//       =====================================================================================
#        if (  ( RSOLVER == HLLE || RSOLVER == HLLC )  &&  defined HLL_NO_REF_STATE  )

//       4-2-a1. evaluate the corrections to the left and right face-centered variables
         for (int v=0; v<NCOMP_FLUID; v++)
         {
            Correct_L[v] = (real)0.0;
            Correct_R[v] = (real)0.0;
         }

         for (int Mode=0; Mode<NCOMP_FLUID; Mode++)
         {
            Coeff_L = (real)0.0;
            Coeff_R = (real)0.0;

            if ( HLL_Include_All_Waves  ||  EigenVal[d][Mode] <= (real)0.0 )
            {
               const real Coeff_C = -dt_dh2*EigenVal[d][Mode];
               const real Coeff_D = real(-4.0/3.0)*SQR(Coeff_C);

               for (int v=0; v<NCOMP_FLUID; v++)   Coeff_L += LEigenVec[Mode][v]*(  Coeff_C*( dfc[v] + dfc6[v] ) +
                                                                                    Coeff_D*( dfc6[v]          )   );

               for (int v=0; v<NCOMP_FLUID; v++)   Correct_L[v] += Coeff_L*REigenVec[Mode][v];
            }

            if ( HLL_Include_All_Waves  ||  EigenVal[d][Mode] >= (real)0.0 )
            {
               const real Coeff_A = -dt_dh2*EigenVal[d][Mode];
               const real Coeff_B = real(-4.0/3.0)*SQR(Coeff_A);

               for (int v=0; v<NCOMP_FLUID; v++)   Coeff_R += LEigenVec[Mode][v]*(  Coeff_A*( dfc[v] - dfc6[v] ) +
                                                                                    Coeff_B*( dfc6[v]          )   );

               for (int v=0; v<NCOMP_FLUID; v++)   Correct_R[v] += Coeff_R*REigenVec[Mode][v];
            }
         } // for (int Mode=0; Mode<NCOMP_FLUID; Mode++)


//       =====================================================================================
//       b. for the Roe's and exact solvers
//       =====================================================================================
#        else // ( RSOLVER == ROE/EXACT || ifndef HLL_NO_REF_STATE )

//       4-2-b1. evaluate the reference states
         Coeff_L = -dt_dh2*FMIN( EigenVal[d][0], (real)0.0 );
         Coeff_R = -dt_dh2*FMAX( EigenVal[d][4], (real)0.0 );

         for (int v=0; v<NCOMP_FLUID; v++)
         {
            Correct_L[v] = Coeff_L*(  dfc[v] + ( (real)1.0 - real(4.0/3.0)*Coeff_L )*dfc6[v]  );
            Correct_R[v] = Coeff_R*(  dfc[v] - ( (real)1.0 + real(4.0/3.0)*Coeff_R )*dfc6[v]  );
         }


//       4-2-b2. evaluate the corrections to the left and right face-centered variables
         for (int Mode=0; Mode<NCOMP_FLUID; Mode++)
         {
            Coeff_L = (real)0.0;
            Coeff_R = (real)0.0;

            if ( EigenVal[d][Mode] <= (real)0.0 )
            {
               const real Coeff_C = dt_dh2*( EigenVal[d][0] - EigenVal[d][Mode] );
//             write as (a-b)*(a+b) instead of a^2-b^2 to ensure that Coeff_D=0 when Coeff_C=0
//             Coeff_D = real(4.0/3.0)*dt_dh2*dt_dh2* ( EigenVal[d][   0]*EigenVal[d][   0] -
//                                                      EigenVal[d][Mode]*EigenVal[d][Mode]   );
               const real Coeff_D = real(4.0/3.0)*dt_dh2*Coeff_C*( EigenVal[d][0] + EigenVal[d][Mode] );

               for (int v=0; v<NCOMP_FLUID; v++)   Coeff_L += LEigenVec[Mode][v]*(  Coeff_C*( dfc[v] + dfc6[v] ) +
                                                                                    Coeff_D*( dfc6[v]          )   );

               for (int v=0; v<NCOMP_FLUID; v++)   Correct_L[v] += Coeff_L*REigenVec[Mode][v];
            }

            if ( EigenVal[d][Mode] >= (real)0.0 )
            {
               const real Coeff_A = dt_dh2*( EigenVal[d][4] - EigenVal[d][Mode] );
//             write as (a-b)*(a+b) instead of a^2-b^2 to ensure that Coeff_B=0 when Coeff_A=0
//             Coeff_B = real(4.0/3.0)*dt_dh2*dt_dh2* ( EigenVal[d][   4]*EigenVal[d][   4] -
//                                                      EigenVal[d][Mode]*EigenVal[d][Mode]   );
               const real Coeff_B = real(4.0/3.0)*dt_dh2*Coeff_A*( EigenVal[d][4] + EigenVal[d][Mode] );

               for (int v=0; v<NCOMP_FLUID; v++)   Coeff_R += LEigenVec[Mode][v]*(  Coeff_A*( dfc[v] - dfc6[v] ) +
                                                                                    Coeff_B*( dfc6[v]          )   );

               for (int v=0; v<NCOMP_FLUID; v++)   Correct_R[v] += Coeff_R*REigenVec[Mode][v];
            }
         } // for (int Mode=0; Mode<NCOMP_FLUID; Mode++)

#        endif // if (  ( RSOLVER == HLLE || RSOLVER == HLLC )  &&  defined HLL_NO_REF_STATE  ) ... else ...


//       4-3. evaluate the corrections to the left and right face-centered passive scalars
//            --> passive scalars travel with fluid velocity (i.e., entropy mode)
#        if ( NCOMP_PASSIVE > 0 )
         Coeff_L = -dt_dh2*FMIN( EigenVal[d][1], (real)0.0 );
         Coeff_R = -dt_dh2*FMAX( EigenVal[d][1], (real)0.0 );

         for (int v=NCOMP_FLUID; v<NCOMP_TOTAL; v++)
         {
            Correct_L[v] = Coeff_L*(  dfc[v] + ( (real)1.0 - real(4.0/3.0)*Coeff_L )*dfc6[v]  );
            Correct_R[v] = Coeff_R*(  dfc[v] - ( (real)1.0 + real(4.0/3.0)*Coeff_R )*dfc6[v]  );
         }
#        endif


//       4-4. evaluate the face-centered variables at the half time-step
         Hydro_Rotate3D( Correct_L, d, false );
         Hydro_Rotate3D( Correct_R, d, false );

         for (int v=0; v<NCOMP_TOTAL; v++)
         {
            fc[faceL][v] += Correct_L[v];
            fc[faceR][v] += Correct_R[v];
         }

//       ensure positive density and pressure
         fc[faceL][0] = FMAX( fc[faceL][0], MinDens );
         fc[faceR][0] = FMAX( fc[faceR][0], MinDens );

         fc[faceL][4] = Hydro_CheckMinPres( fc[faceL][4], MinPres );
         fc[faceR][4] = Hydro_CheckMinPres( fc[faceR][4], MinPres );

#        if ( NCOMP_PASSIVE > 0 )
         for (int v=NCOMP_FLUID; v<NCOMP_TOTAL; v++) {
         fc[faceL][v] = FMAX( fc[faceL][v], TINY_NUMBER );
         fc[faceR][v] = FMAX( fc[faceR][v], TINY_NUMBER ); }
#        endif

#        endif // #if ( FLU_SCHEME == CTU )


//       5. primitive variables --> conserved variables
         real tmp[NCOMP_TOTAL];  // input and output arrays must not overlap for Pri2Con()

         for (int v=0; v<NCOMP_TOTAL; v++)   tmp[v] = fc[faceL][v];
         Hydro_Pri2Con( tmp, fc[faceL], _Gamma_m1, NormPassive, NNorm, NormIdx );

         for (int v=0; v<NCOMP_TOTAL; v++)   tmp[v] = fc[faceR][v];
         Hydro_Pri2Con( tmp, fc[faceR], _Gamma_m1, NormPassive, NNorm, NormIdx );

      } // for (int d=0; d<3; d++)


#     if ( FLU_SCHEME == MHM )
//    6. advance the face-centered variables by half time-step for the MHM integrator
      Hydro_HancockPredict( fc, dt, dh, Gamma_m1, _Gamma_m1, g_ConVar, idx_cc, MinDens, MinPres );
#     endif


//    7. store the face-centered values to the output array
      for (int f=0; f<6; f++)
      for (int v=0; v<NCOMP_TOTAL; v++)
         g_FC_Var[f][v][idx_fc] = fc[f][v];

   } // CGPU_LOOP( idx_fc, CUBE(NOut) )


#  ifdef __CUDACC__
   __syncthreads();
#  endif

} // FUNCTION : Hydro_DataReconstruction (PPM)
#endif // #if ( LR_SCHEME == PPM )



#ifdef CHAR_RECONSTRUCTION
//-------------------------------------------------------------------------------------------------------
// Function    :  Hydro_Pri2Char
// Description :  Primitive variables --> characteristic variables
//
// Note           1. Passive scalars require no conversion
//                   --> Their eigenmatrices are just identity matrix
//                2. Input and output share the same array
//                3. InOut[] should have the size of NCOMP_TOTAL_PLUS_MAG
//
// Parameter   :  InOut     : Array storing both the input primitive variables and output characteristic variables
//                Gamma     : Ratio of specific heats (for pure hydro only)
//                Rho       : Density                 (for pure hydro only)
//                Pres      : Pressure                (for pure hydro only)
//                LEigenVec : Left eigenvector        (for MHD only)
//                XYZ       : Target spatial direction : (0/1/2) --> (x/y/z)
//-------------------------------------------------------------------------------------------------------
GPU_DEVICE
void Hydro_Pri2Char( real InOut[], const real Gamma, const real Rho, const real Pres,
                     const real LEigenVec[][NWAVE], const int XYZ )
{

// check
#  ifdef CHECK_NEGATIVE_IN_FLUID
   if ( Hydro_CheckNegative(Pres) )
      printf( "ERROR : invalid pressure (%14.7e) at file <%s>, line <%d>, function <%s>\n",
              Pres, __FILE__, __LINE__, __FUNCTION__ );

   if ( Hydro_CheckNegative(Rho) )
      printf( "ERROR : invalid density (%14.7e) at file <%s>, line <%d>, function <%s>\n",
              Rho,  __FILE__, __LINE__, __FUNCTION__ );
#  endif


// back-up the input array and rotate it according to the target direction
// --> it's unnecessary to copy the passive scalars since they will not be modified
   real Temp[ NCOMP_FLUID + NCOMP_MAGNETIC ];

   for (int v=0; v<NCOMP_FLUID; v++)   Temp[v] = InOut[v];

#  ifdef MHD
   for (int v=NCOMP_FLUID; v<NCOMP_FLUID+NCOMP_MAGNETIC; v++)  Temp[v] = InOut[ v - NCOMP_FLUID + MAG_OFFSET ];
#  endif

   Hydro_Rotate3D( Temp, XYZ, true, NCOMP_FLUID );

// remove the normal B field to be consistent with the eigenvector matrix
#  ifdef MHD
   Temp[ NCOMP_FLUID + 0 ] = Temp[ NCOMP_FLUID + 1 ];
   Temp[ NCOMP_FLUID + 1 ] = Temp[ NCOMP_FLUID + 2 ];
#  endif


// primitive --> characteristic
// a. MHD
#  ifdef MHD
   const real tmp_f1 = LEigenVec[0][1]*Temp[1] + LEigenVec[0][2]*Temp[2] + LEigenVec[0][3]*Temp[3];
   const real tmp_b1 = LEigenVec[0][4]*Temp[4] + LEigenVec[0][5]*Temp[5] + LEigenVec[0][6]*Temp[6];
   const real tmp_f2 = LEigenVec[2][1]*Temp[1] + LEigenVec[2][2]*Temp[2] + LEigenVec[2][3]*Temp[3];
   const real tmp_b2 = LEigenVec[2][4]*Temp[4] + LEigenVec[2][5]*Temp[5] + LEigenVec[2][6]*Temp[6];

   InOut[MAG_OFFSET+0] = (real)0.0;
   InOut[           3] = Temp[0] + LEigenVec[3][4]*Temp[4];
   InOut[           1] = LEigenVec[1][2]*Temp[2] + LEigenVec[1][3]*Temp[3] + LEigenVec[1][5]*Temp[5] + LEigenVec[1][6]*Temp[6];
   InOut[MAG_OFFSET+1] = LEigenVec[5][2]*Temp[2] + LEigenVec[5][3]*Temp[3] + LEigenVec[5][5]*Temp[5] + LEigenVec[5][6]*Temp[6];
   InOut[           0] =  tmp_f1 + tmp_b1;
   InOut[           2] =  tmp_f2 + tmp_b2;
   InOut[           4] = -tmp_f2 + tmp_b2;
   InOut[MAG_OFFSET+2] = -tmp_f1 + tmp_b1;

// b. pure hydro
#  else // #ifdef MHD
   const real _a2 = (real)1.0 / ( Gamma*Pres/Rho );
   const real _a  = SQRT( _a2 );

   InOut[0] = -(real)0.5*Rho*_a*Temp[1] + (real)0.5*_a2*Temp[4];
   InOut[1] = Temp[0] - _a2*Temp[4];
   InOut[2] = Temp[2];
   InOut[3] = Temp[3];
   InOut[4] = +(real)0.5*Rho*_a*Temp[1] + (real)0.5*_a2*Temp[4];
#  endif // #ifdef MHD ... else ...

} // FUNCTION : Hydro_Pri2Char



//-------------------------------------------------------------------------------------------------------
// Function    :  Hydro_Char2Pri
// Description :  Characteristic variables --> primitive variables
//
// Note           1. Passive scalars require no conversion
//                   --> Their eigenmatrices are just identity matrix
//                2. Input and output share the same array
//
// Parameter   :  InOut     : Array storing both the input characteristic variables and output primitive variables
//                Gamma     : Ratio of specific heats (for pure hydro only)
//                Rho       : Density                 (for pure hydro only)
//                Pres      : Pressure                (for pure hydro only)
//                REigenVec : Right eigenvector       (for MHD only)
//                XYZ       : Target spatial direction : (0/1/2) --> (x/y/z)
//-------------------------------------------------------------------------------------------------------
GPU_DEVICE
void Hydro_Char2Pri( real InOut[], const real Gamma, const real Rho, const real Pres,
                     const real REigenVec[][NWAVE], const int XYZ )
{

// check
#  ifdef CHECK_NEGATIVE_IN_FLUID
   if ( Hydro_CheckNegative(Pres) )
      printf( "ERROR : negative pressure (%14.7e) at file <%s>, line <%d>, function <%s>\n",
              Pres, __FILE__, __LINE__, __FUNCTION__ );

   if ( Hydro_CheckNegative(Rho) )
      printf( "ERROR : negative density (%14.7e) at file <%s>, line <%d>, function <%s>\n",
              Rho,  __FILE__, __LINE__, __FUNCTION__ );
#  endif


// back-up the input array and rotate it according to the target direction
// --> it's unnecessary to copy the passive scalars since they will not be modified
// --> it's also unnecessary to copy the normal B field (just to be consistent with the eigenvector matrix)
   real Temp[ NCOMP_FLUID + NCOMP_MAGNETIC - 1 ];

   for (int v=0; v<NCOMP_FLUID; v++)   Temp[v] = InOut[v];

#  ifdef MHD
   for (int v=NCOMP_FLUID; v<NCOMP_FLUID+NCOMP_MAGNETIC-1; v++)  Temp[v] = InOut[ v - NCOMP_FLUID + MAG_OFFSET + 1 ];
#  endif


// primitive --> characteristic
   const real _Rho = (real)1.0 / Rho;
   const real a2   = Gamma*Pres*_Rho;

// a. MHD
#  ifdef MHD
   InOut[           0] = REigenVec[0][0]*Temp[0] + REigenVec[2][0]*Temp[2] + Temp[3] +
                         REigenVec[4][0]*Temp[4] + REigenVec[6][0]*Temp[6];
   InOut[           1] = REigenVec[0][1]*Temp[0] + REigenVec[2][1]*Temp[2] + REigenVec[4][1]*Temp[4] +
                         REigenVec[6][1]*Temp[6];
   InOut[           2] = REigenVec[0][2]*Temp[0] + REigenVec[1][2]*Temp[1] + REigenVec[2][2]*Temp[2] +
                         REigenVec[4][2]*Temp[4] + REigenVec[5][2]*Temp[5] + REigenVec[6][2]*Temp[6];
   InOut[           3] = REigenVec[0][3]*Temp[0] + REigenVec[1][3]*Temp[1] + REigenVec[2][3]*Temp[2] +
                         REigenVec[4][3]*Temp[4] + REigenVec[5][3]*Temp[5] + REigenVec[6][3]*Temp[6];
   InOut[           4] = ( InOut[0] - Temp[3] )*a2;
   InOut[MAG_OFFSET+0] = (real)0.0;
   InOut[MAG_OFFSET+1] = REigenVec[0][5]*Temp[0] + REigenVec[1][5]*Temp[1] + REigenVec[2][5]*Temp[2] +
                         REigenVec[4][5]*Temp[4] + REigenVec[5][5]*Temp[5] + REigenVec[6][5]*Temp[6];
   InOut[MAG_OFFSET+2] = REigenVec[0][6]*Temp[0] + REigenVec[1][6]*Temp[1] + REigenVec[2][6]*Temp[2] +
                         REigenVec[4][6]*Temp[4] + REigenVec[5][6]*Temp[5] + REigenVec[6][6]*Temp[6];

// b. pure hydro
#  else // #ifdef MHD
   const real a = SQRT( a2 );

   InOut[0] = Temp[0] + Temp[1] + Temp[4];
   InOut[1] = a*_Rho*( -Temp[0] + Temp[4] );
   InOut[2] = Temp[2];
   InOut[3] = Temp[3];
   InOut[4] = a2*( Temp[0] + Temp[4] );
#  endif // #ifdef MHD ... else ...

   Hydro_Rotate3D( InOut, XYZ, false, MAG_OFFSET );

} // FUNCTION : Hydro_Char2Pri
#endif



#if (  FLU_SCHEME == CTU  ||  ( defined MHD && defined CHAR_RECONSTRUCTION )  )
//-------------------------------------------------------------------------------------------------------
// Function    :  Hydro_GetEigenSystem
// Description :  Evaluate the eigenvalues and left/right eigenvectors
//
// Note        :  1. Input data must be primitive variables
//                2. Constant components of eigenvectors must be set in advance
//                3. Work for the CTU scheme and the characteristic data reconstruction in MHD
//                4. Do not need to consider passive scalars
//                   --> Their eigenmatrices are just identity matrix
//                5. For pure hydro, this function computes the eigenvalues and eigenvectors
//                   along all three spatial directions at once
//                   --> Because eigenvectors along different directions are the same for pure hydro
//                   But for MHD, this function only computes the eigenvalues and eigenvectors
//                   along the spatial direction specified by XYZ
//                   --> Because eigenvectors along different directions are different for MHD
//
// Parameter   :  CC_Var      : Array storing the input cell-centered primitive variables
//                EigenVal    : Array to store the output eigenvalues
//                              --> Hydro: along all three spatial directions
//                                  MHD  : only along the target spatial direction
//                L/REigenVec : Array to store the output left/right eigenvectors
//                Gamma       : Ratio of specific heats
//                XYZ         : Target spatial direction (for MHD only)
//-------------------------------------------------------------------------------------------------------
GPU_DEVICE
#ifdef MHD
void   MHD_GetEigenSystem( const real CC_Var[], real EigenVal[],
                           real LEigenVec[][NWAVE], real REigenVec[][NWAVE],
                           const real Gamma, const int XYZ )
#else
void Hydro_GetEigenSystem( const real CC_Var[], real EigenVal[][NWAVE],
                           real LEigenVec[][NWAVE], real REigenVec[][NWAVE],
                           const real Gamma )
#endif
{

#  ifdef CHECK_NEGATIVE_IN_FLUID
   if ( Hydro_CheckNegative(CC_Var[4]) )
      printf( "ERROR : invalid pressure (%14.7e) at file <%s>, line <%d>, function <%s>\n",
              CC_Var[4], __FILE__, __LINE__, __FUNCTION__ );

   if ( Hydro_CheckNegative(CC_Var[0]) )
      printf( "ERROR : invalid density (%14.7e) at file <%s>, line <%d>, function <%s>\n",
              CC_Var[0], __FILE__, __LINE__, __FUNCTION__ );
#  endif

   const real  Rho = CC_Var[0];
   const real _Rho = (real)1.0/Rho;
   const real  a2  = Gamma*CC_Var[4]*_Rho;
   const real  a   = SQRT( a2 );
   const real _a   = (real)1.0/a;
   const real _a2  = _a*_a;

// a. MHD
#  ifdef MHD
   real Cf2, Cs2, Cf, Cs;
   real PriVar[NCOMP_TOTAL_PLUS_MAG];

   for (int v=0; v<NCOMP_TOTAL_PLUS_MAG; v++)  PriVar[v] = CC_Var[v];

   Hydro_Rotate3D( PriVar, XYZ, true, MAG_OFFSET );

   const real Bx      = PriVar[ MAG_OFFSET + 0 ];
   const real By      = PriVar[ MAG_OFFSET + 1 ];
   const real Bz      = PriVar[ MAG_OFFSET + 2 ];
   const real Bn2     = SQR( By ) + SQR( Bz );
   const real Bn      = SQRT( Bn2 );
   const real Cax2    = SQR( Bx )*_Rho;
   const real Cax     = SQRT( Cax2 );
   const real Cat2    = Bn2*_Rho;
   const real tsum    = Cax2 + Cat2 + a2;                         // Ca^2 + a^2
   const real tdif    = Cax2 + Cat2 - a2;                         // Ca^2 - a^2
   const real Cf2mCs2 = SQRT( SQR(tdif) + (real)4.0*a2*Cat2 );    // Cf^2 - Cs^2

// evaluate the fast/slow wave speed (Cf/Cs)
   if ( Cat2 == (real)0.0 )
   {
      if ( Cax2 == a2 ) {
         Cf2 = a2;
         Cs2 = a2;
      }
      else if( Cax2 > a2 ) {
         Cf2 = Cax2;
         Cs2 = a2;
      }
      else {
         Cf2 = a2;
         Cs2 = Cax2;
      }
   }

   else
   {
      if ( Cax2 == (real)0.0 ) {
         Cf2 = a2 + Cat2;
         Cs2 = (real)0.0;
      }
      else {
         Cf2 = (real)0.5*( tsum + Cf2mCs2 );
         Cs2 = Cf2 - Cf2mCs2;
      }
   } // if ( Cat2 == (real)0.0 ) ... else ...

   Cf = SQRT( Cf2 );
   Cs = SQRT( Cs2 );


// eigenvalues along the target spatial direction
   EigenVal[0] = PriVar[1] - Cf;
   EigenVal[1] = PriVar[1] - Cax;
   EigenVal[2] = PriVar[1] - Cs;
   EigenVal[3] = PriVar[1];
   EigenVal[4] = PriVar[1] + Cs;
   EigenVal[5] = PriVar[1] + Cax;
   EigenVal[6] = PriVar[1] + Cf;


// right eigenvectors (rows instead of columns of the matrix REigenVec for better performance)
   const real S         = SIGN( Bx );
   const real sqrt_Rho  = SQRT( Rho );
   const real _sqrt_Rho = (real)1.0 / sqrt_Rho;
   const real a2mCs2    = a2 - Cs2;
   const real Cf2ma2    = Cf2 - a2;

   real beta_y, beta_z, alpha_f, alpha_s;

   if ( Bn == (real)0.0 ) {
      beta_y = (real)1.0;
      beta_z = (real)0.0;
   }
   else {
      const real _Bn = (real)1.0 / Bn;
      beta_y = PriVar[6] * _Bn;
      beta_z = PriVar[7] * _Bn;
   }

   if ( Cf2mCs2 == (real)0.0 ) {
      alpha_f = (real)1.0;
      alpha_s = (real)0.0;
   }
   else if ( a2mCs2 <= (real)0.0 ) {
      alpha_f = (real)0.0;
      alpha_s = (real)1.0;
   }
   else if ( Cf2ma2 <= (real)0.0 ) {
      alpha_f = (real)1.0;
      alpha_s = (real)0.0;
   }
   else {
      const real _Cf2mCs2 = (real)1.0 / Cf2mCs2;
      alpha_f = SQRT( a2mCs2*_Cf2mCs2 );
      alpha_s = SQRT( Cf2ma2*_Cf2mCs2 );
   }

   const real Af         = a * alpha_f * sqrt_Rho;
   const real As         = a * alpha_s * sqrt_Rho;
   const real Cff        = Cf * alpha_f;
   const real Css        = Cs * alpha_s;
   const real Qf         = Cff * S;
   const real Qs         = Css * S;
   const real S_sqrt_Rho = S * sqrt_Rho;

   REigenVec[0][0] =  Rho * alpha_f;
   REigenVec[0][1] = -Cff;
   REigenVec[0][2] =  Qs * beta_y;
   REigenVec[0][3] =  Qs * beta_z;
   REigenVec[0][4] =  REigenVec[0][0] * a2;
   REigenVec[0][5] =  As * beta_y;
   REigenVec[0][6] =  As * beta_z;

   REigenVec[1][2] = -beta_z;
   REigenVec[1][3] =  beta_y;
   REigenVec[1][5] = -S_sqrt_Rho * beta_z;
   REigenVec[1][6] =  S_sqrt_Rho * beta_y;

   REigenVec[2][0] =  Rho * alpha_s;
   REigenVec[2][1] = -Css;
   REigenVec[2][2] = -Qf * beta_y;
   REigenVec[2][3] = -Qf * beta_z;
   REigenVec[2][4] =  REigenVec[2][0] * a2;
   REigenVec[2][5] = -Af * beta_y;
   REigenVec[2][6] = -Af * beta_z;

   REigenVec[4][0] =  REigenVec[2][0];
   REigenVec[4][1] = -REigenVec[2][1];
   REigenVec[4][2] = -REigenVec[2][2];
   REigenVec[4][3] = -REigenVec[2][3];
   REigenVec[4][4] =  REigenVec[2][4];
   REigenVec[4][5] =  REigenVec[2][5];
   REigenVec[4][6] =  REigenVec[2][6];

   REigenVec[5][2] = -REigenVec[1][2];
   REigenVec[5][3] = -REigenVec[1][3];
   REigenVec[5][5] =  REigenVec[1][5];
   REigenVec[5][6] =  REigenVec[1][6];

   REigenVec[6][0] =  REigenVec[0][0];
   REigenVec[6][1] = -REigenVec[0][1];
   REigenVec[6][2] = -REigenVec[0][2];
   REigenVec[6][3] = -REigenVec[0][3];
   REigenVec[6][4] =  REigenVec[0][4];
   REigenVec[6][5] =  REigenVec[0][5];
   REigenVec[6][6] =  REigenVec[0][6];


// left eigenvectors (rows of the matrix LEigenVec)
   const real N         = (real)0.5 * _a2;
   const real N_By      = N * beta_y;
   const real N_Bz      = N * beta_z;
   const real As_Rho    = As * _Rho;
   const real Af_Rho    = Af * _Rho;
   const real S_inv_Rho = S * _sqrt_Rho;

   LEigenVec[0][1] = -N * Cff;
   LEigenVec[0][2] =  N_By * Qs;
   LEigenVec[0][3] =  N_Bz * Qs;
   LEigenVec[0][4] =  N * alpha_f * _Rho;
   LEigenVec[0][5] =  N_By * As_Rho;
   LEigenVec[0][6] =  N_Bz * As_Rho;

   LEigenVec[1][2] = -(real)0.5 * beta_z;
   LEigenVec[1][3] =  (real)0.5 * beta_y;
   LEigenVec[1][5] =  LEigenVec[1][2] * S_inv_Rho;
   LEigenVec[1][6] =  LEigenVec[1][3] * S_inv_Rho;

   LEigenVec[2][1] = -N * Css;
   LEigenVec[2][2] = -N_By * Qf;
   LEigenVec[2][3] = -N_Bz * Qf;
   LEigenVec[2][4] =  N * alpha_s * _Rho;
   LEigenVec[2][5] = -N_By * Af_Rho;
   LEigenVec[2][6] = -N_Bz * Af_Rho;

   LEigenVec[3][4] = -_a2;

   LEigenVec[4][1] = -LEigenVec[2][1];
   LEigenVec[4][2] = -LEigenVec[2][2];
   LEigenVec[4][3] = -LEigenVec[2][3];
   LEigenVec[4][4] =  LEigenVec[2][4];
   LEigenVec[4][5] =  LEigenVec[2][5];
   LEigenVec[4][6] =  LEigenVec[2][6];

   LEigenVec[5][2] = -LEigenVec[1][2];
   LEigenVec[5][3] = -LEigenVec[1][3];
   LEigenVec[5][5] =  LEigenVec[1][5];
   LEigenVec[5][6] =  LEigenVec[1][6];

   LEigenVec[6][1] = -LEigenVec[0][1];
   LEigenVec[6][2] = -LEigenVec[0][2];
   LEigenVec[6][3] = -LEigenVec[0][3];
   LEigenVec[6][4] =  LEigenVec[0][4];
   LEigenVec[6][5] =  LEigenVec[0][5];
   LEigenVec[6][6] =  LEigenVec[0][6];


// b. pure hydro
#  else // #ifdef MHD

   const real vx = CC_Var[1];
   const real vy = CC_Var[2];
   const real vz = CC_Var[3];

// eigenvalues along all three spatial directions
   EigenVal[0][0] = vx - a;
   EigenVal[0][1] = vx;
   EigenVal[0][2] = vx;
   EigenVal[0][3] = vx;
   EigenVal[0][4] = vx + a;

   EigenVal[1][0] = vy - a;
   EigenVal[1][1] = vy;
   EigenVal[1][2] = vy;
   EigenVal[1][3] = vy;
   EigenVal[1][4] = vy + a;

   EigenVal[2][0] = vz - a;
   EigenVal[2][1] = vz;
   EigenVal[2][2] = vz;
   EigenVal[2][3] = vz;
   EigenVal[2][4] = vz + a;


// NOTE : the left and right eigenvectors have the same form along different spatial directions for hydro
// left eigenvectors (rows of the matrix LEigenVec)
   LEigenVec[0][1] = -(real)0.5*Rho*_a;
   LEigenVec[0][4] = (real)0.5*_a2;
   LEigenVec[1][4] = -_a2;
   LEigenVec[4][1] = -LEigenVec[0][1];
   LEigenVec[4][4] = +LEigenVec[0][4];


// right eigenvectors (rows instead of columns of the matrix REigenVec for better performance)
   REigenVec[0][1] = -a*_Rho;
   REigenVec[0][4] = a2;
   REigenVec[4][1] = -REigenVec[0][1];
   REigenVec[4][4] = a2;

#  endif // #ifdef MHD ... else ...

} // FUNCTION : Hydro/MHD_GetEigenSystem
#endif // #if (  FLU_SCHEME == CTU  ||  ( defined MHD && defined CHAR_RECONSTRUCTION )  )



//-------------------------------------------------------------------------------------------------------
// Function    :  Hydro_LimitSlope
// Description :  Evaluate the monotonic slope by slope limiters
//
// Note        :  1. Input data must be primitive variables
//
// Parameter   :  L             : Element x-1
//                C             : Element x
//                R             : Element x+1
//                LR_Limiter    : Slope limiter for the data reconstruction in the MHM/MHM_RP/CTU schemes
//                                (0/1/2/3) = (vanLeer/generalized MinMod/vanAlbada/vanLeer+generalized MinMod) limiter
//                MinMod_Coeff  : Coefficient of the generalized MinMod limiter
//                Gamma         : Ratio of specific heats
//                                --> For pure hydro + CHAR_RECONSTRUCTION only
//                XYZ           : Target spatial direction : (0/1/2) --> (x/y/z)
//                                --> For CHAR_RECONSTRUCTION only
//                L/REigenVec   : Array storing the left/right eigenvectors
//                                --> For MHD + CHAR_RECONSTRUCTION only
//                Slope_Limiter : Array to store the output monotonic slope
//-------------------------------------------------------------------------------------------------------
GPU_DEVICE
void Hydro_LimitSlope( const real L[], const real C[], const real R[], const LR_Limiter_t LR_Limiter,
                       const real MinMod_Coeff, const real Gamma, const int XYZ,
                       const real LEigenVec[][NWAVE], const real REigenVec[][NWAVE],
                       real Slope_Limiter[] )
{

// check
#  ifdef GAMER_DEBUG
#  if ( defined MHD  &&  defined CHAR_RECONSTRUCTION )
   if ( LEigenVec == NULL )   printf( "ERROR : LEigenVec == NULL !!\n" );
   if ( REigenVec == NULL )   printf( "ERROR : REigenVec == NULL !!\n" );
#  endif
#  endif


   real Slope_L[NCOMP_TOTAL_PLUS_MAG], Slope_R[NCOMP_TOTAL_PLUS_MAG], Slope_C[NCOMP_TOTAL_PLUS_MAG];
   real Slope_A[NCOMP_TOTAL_PLUS_MAG], Slope_LR;

// evaluate different slopes
   for (int v=0; v<NCOMP_TOTAL_PLUS_MAG; v++)
   {
      Slope_L[v] = C[v] - L[v];
      Slope_R[v] = R[v] - C[v];
      Slope_C[v] = (real)0.5*( Slope_L[v] + Slope_R[v] );
   }

   if ( LR_Limiter == VL_GMINMOD )
   {
      for (int v=0; v<NCOMP_TOTAL_PLUS_MAG; v++)
      {
         if ( Slope_L[v]*Slope_R[v] > (real)0.0 )
            Slope_A[v] = (real)2.0*Slope_L[v]*Slope_R[v]/( Slope_L[v] + Slope_R[v] );
         else
            Slope_A[v] = (real)0.0;
      }
   }


// primitive variables --> characteristic variables
#  ifdef CHAR_RECONSTRUCTION
#  ifdef MHD
   const real Rho  = NULL_REAL;
   const real Pres = NULL_REAL;
#  else
   const real Rho  = C[0];
   const real Pres = C[4];
#  endif

   Hydro_Pri2Char( Slope_L, Gamma, Rho, Pres, LEigenVec, XYZ );
   Hydro_Pri2Char( Slope_R, Gamma, Rho, Pres, LEigenVec, XYZ );
   Hydro_Pri2Char( Slope_C, Gamma, Rho, Pres, LEigenVec, XYZ );

   if ( LR_Limiter == VL_GMINMOD )
      Hydro_Pri2Char( Slope_A, Gamma, Rho, Pres, LEigenVec, XYZ );
#  endif


// apply the slope limiter
   for (int v=0; v<NCOMP_TOTAL_PLUS_MAG; v++)
   {
      Slope_LR = Slope_L[v]*Slope_R[v];

      if ( Slope_LR > (real)0.0 )
      {
         switch ( LR_Limiter )
         {
            case VANLEER:     // van-Leer
               Slope_Limiter[v] = (real)2.0*Slope_LR/( Slope_L[v] + Slope_R[v] );
               break;

            case GMINMOD:     // generalized MinMod
               Slope_L[v] *= MinMod_Coeff;
               Slope_R[v] *= MinMod_Coeff;
               Slope_Limiter[v]  = FMIN(  FABS( Slope_L[v] ), FABS( Slope_R[v] )  );
               Slope_Limiter[v]  = FMIN(  FABS( Slope_C[v] ), Slope_Limiter[v]  );
               Slope_Limiter[v] *= SIGN( Slope_C[v] );
               break;

            case ALBADA:      // van-Albada
               Slope_Limiter[v] = Slope_LR*( Slope_L[v] + Slope_R[v] ) /
                                  ( Slope_L[v]*Slope_L[v] + Slope_R[v]*Slope_R[v] );
               break;

            case VL_GMINMOD:  // van-Leer + generalized MinMod
               Slope_L[v] *= MinMod_Coeff;
               Slope_R[v] *= MinMod_Coeff;
               Slope_Limiter[v]  = FMIN(  FABS( Slope_L[v] ), FABS( Slope_R[v] )  );
               Slope_Limiter[v]  = FMIN(  FABS( Slope_C[v] ), Slope_Limiter[v]  );
               Slope_Limiter[v]  = FMIN(  FABS( Slope_A[v] ), Slope_Limiter[v]  );
               Slope_Limiter[v] *= SIGN( Slope_C[v] );
               break;

            default :
#              ifdef GAMER_DEBUG
               printf( "ERROR : incorrect parameter %s = %d !!\n", "LR_Limiter", LR_Limiter );
#              endif
               return;
         }
      } // if ( Slope_LR > (real)0.0 )

      else
      {
         Slope_Limiter[v] = (real)0.0;
      } // if ( Slope_LR > (real)0.0 ) ... else ...
   } // for (int v=0; v<NCOMP_TOTAL_PLUS_MAG; v++)


// characteristic variables --> primitive variables
#  ifdef CHAR_RECONSTRUCTION
   Hydro_Char2Pri( Slope_Limiter, Gamma, Rho, Pres, REigenVec, XYZ );
#  endif

} // FUNCTION : Hydro_LimitSlope



#if ( FLU_SCHEME == MHM )
//-------------------------------------------------------------------------------------------------------
// Function    :  Hydro_HancockPredict
// Description :  Evolve the face-centered variables by half time-step by calculating the face-centered fluxes
//                (no Riemann solver is required)
//
// Note        :  1. Work for the MHM scheme
//                2. Do NOT require data in the neighboring cells
//                3. Input variables must be conserved variables
//
// Parameter   :  fc           : Face-centered conserved variables to be updated
//                dt           : Time interval to advance solution
//                dh           : Cell size
//                Gamma_m1     : Gamma - 1
//                _Gamma_m1    : 1 / (Gamma - 1)
//                g_cc_array   : Array storing the cell-centered conserved variables for checking
//                               negative density and pressure
//                               --> It is just the input array Flu_Array_In[]
//                cc_idx       : Index for accessing g_cc_array[]
//                MinDens/Pres : Minimum allowed density and pressure
//-------------------------------------------------------------------------------------------------------
GPU_DEVICE
void Hydro_HancockPredict( real fc[][NCOMP_TOTAL], const real dt, const real dh,
                           const real Gamma_m1, const real _Gamma_m1,
                           const real g_cc_array[][ CUBE(FLU_NXT) ], const int cc_idx,
                           const real MinDens, const real MinPres )
{

   const real dt_dh2 = (real)0.5*dt/dh;

   real Flux[6][NCOMP_TOTAL], dFlux;


// calculate flux
   for (int f=0; f<6; f++)    Hydro_Con2Flux( f/2, Flux[f], fc[f], Gamma_m1, MinPres );

// update the face-centered variables
   for (int v=0; v<NCOMP_TOTAL; v++)
   {
      dFlux = dt_dh2*( Flux[1][v] - Flux[0][v] + Flux[3][v] - Flux[2][v] + Flux[5][v] - Flux[4][v] );

      for (int f=0; f<6; f++)    fc[f][v] -= dFlux;
   }

// check the negative density and energy
   for (int f=0; f<6; f++)
   {
      if ( fc[f][0] <= (real)0.0  ||  fc[f][4] <= (real)0.0 )
      {
//       set to the cell-centered values before update
         for (int f=0; f<6; f++)
         for (int v=0; v<NCOMP_TOTAL; v++)
            fc[f][v] = g_cc_array[v][cc_idx];

         break;
      }
   }

// ensure positive density and pressure
   for (int f=0; f<6; f++)
   {
#     ifdef MHD
#     warning : WAIT MHD !!!
      const real EngyB = NULL_REAL;
#     else
      const real EngyB = NULL_REAL;
#     endif
      fc[f][0] = FMAX( fc[f][0], MinDens );
      fc[f][4] = Hydro_CheckMinPresInEngy( fc[f][0], fc[f][1], fc[f][2], fc[f][3], fc[f][4],
                                           Gamma_m1, _Gamma_m1, MinPres, EngyB );
#     if ( NCOMP_PASSIVE > 0 )
      for (int v=NCOMP_FLUID; v<NCOMP_TOTAL; v++)
      fc[f][v] = FMAX( fc[f][v], TINY_NUMBER );
#     endif
   }

} // FUNCTION : Hydro_HancockPredict
#endif // #if ( FLU_SCHEME == MHM )



// MINMOD macro is only used in this function
#ifdef MINMOD
#  undef MINMOD
#endif



#endif // #if (  MODEL == HYDRO  &&  ( FLU_SCHEME == MHM || FLU_SCHEME == MHM_RP || FLU_SCHEME == CTU )  )



#endif // #ifndef __CUFLU_DATARECONSTRUCTION__
