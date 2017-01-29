/*
   Copyright (c) 2009-2017, Jack Poulson
   All rights reserved.

   This file is part of Elemental and is under the BSD 2-Clause License,
   which can be found in the LICENSE file in the root directory, or at
   http://opensource.org/licenses/BSD-2-Clause
*/
#include <El.hpp>
#include "./util.hpp"

namespace El {

// TODO(poulson): Move this into a central location.
template<typename Real>
Real RelativeComplementarityGap
( const Real& primalObj, const Real& dualObj, const Real& dualityProduct )
{
    EL_DEBUG_CSE
    Real relCompGap;
    if( primalObj < Real(0) )
        relCompGap = dualityProduct / -primalObj;
    else if( dualObj > Real(0) )
        relCompGap = dualityProduct / dualObj;
    else
        relCompGap = 2; // 200% error if the signs differ inadmissibly.
    return relCompGap;
}

// TODO(poulson): Move this into a central location.
template<typename Real>
Real RelativeObjectiveGap
( const Real& primalObj, const Real& dualObj, const Real& dualityProduct )
{
    EL_DEBUG_CSE
    const Real relObjGap =
      Abs(primalObj-dualObj) / (Max(Abs(primalObj),Abs(dualObj))+1);
    return relObjGap;
}

namespace qp {
namespace affine {

// The following solves a pair of quadratic programs in "affine" conic form:
//
//   min (1/2) x^T Q x + c^T x
//   s.t. A x = b, G x + s = h, s >= 0,
//
//   max (1/2) (A^T y + G^T z + c)^T pinv(Q) (A^T y + G^T z + c) - b^T y - h^T z
//   s.t. A^T y + G^T z + c in range(Q), z >= 0,
//
// as opposed to the more specific "direct" conic form:
//
//   min (1/2) x^T Q x + c^T x
//   s.t. A x = b, x >= 0,
//
//   max (1/2) (A^T y - z + c)^T pinv(Q) (A^T y - z + c) - b^T y
//   s.t. A^T y - z + c in range(Q), z >= 0,
//
// which corresponds to G = -I and h = 0.
//
// We make use of the regularized Lagrangian
//
//   L(x,s;y,z) = (1/2) x^T Q x + c^T x + y^T (A x - b) + z^T (G x + s - h)
//                + (1/2) gamma_x || x - x_0 ||_2^2
//                + (1/2) gamma_s || s - s_0 ||_2^2
//                - (1/2) gamma_y || y - y_0 ||_2^2
//                - (1/2) gamma_z || z - z_0 ||_2^2
//                + mu Phi(s).
//
// where we note that the two-norm regularization is positive for the primal
// variable x and *negative* for the dual variables y and z. There is not yet
// any regularization on the primal slack variable s (though it may be
// investigated in the future).
//
// The subsequent first-order optimality conditions for x, y, and z become
//
//   Nabla_x L = Q x + c + A^T y + G^T z + gamma_x (x - x_0) = 0,
//   Nabla_y L = A x - b - gamma_y (y - y_0) = 0,
//   Nabla_z L = G x + s - h - gamma_z (z - z_0) = 0.
//
// These can be arranged into the symmetric quasi-definite form
//
//   | Q + gamma_x I,      A^T,      G^T     | | x | = | -c + gamma_x x_0  |
//   |        A,      -gamma_y I,     0      | | y |   |  b - gamma_y y_0  |
//   |        G,            0,    -gamma_z I | | z |   | h-s - gamma_z z_0 |.
//

template<typename Real>
void IPM
( const Matrix<Real>& QPre,
  const Matrix<Real>& APre,
  const Matrix<Real>& GPre,
  const Matrix<Real>& bPre,
  const Matrix<Real>& cPre,
  const Matrix<Real>& hPre,
        Matrix<Real>& x,
        Matrix<Real>& y,
        Matrix<Real>& z,
        Matrix<Real>& s,
  const IPMCtrl<Real>& ctrl )
{
    EL_DEBUG_CSE

    // Equilibrate the QP by diagonally scaling [A;G]
    auto A = APre;
    auto G = GPre;
    auto b = bPre;
    auto c = cPre;
    auto h = hPre;
    auto Q = QPre;
    const Int m = A.Height();
    const Int k = G.Height();
    const Int n = A.Width();
    const Int degree = k;
    Matrix<Real> dRowA, dRowG, dCol;
    if( ctrl.outerEquil )
    {
        StackedRuizEquil( A, G, dRowA, dRowG, dCol, ctrl.print );
        DiagonalSolve( LEFT, NORMAL, dRowA, b );
        DiagonalSolve( LEFT, NORMAL, dRowG, h );
        DiagonalSolve( LEFT, NORMAL, dCol,  c );
        // TODO(poulson): Replace with SymmetricDiagonalSolve
        {
            DiagonalSolve( LEFT, NORMAL, dCol,  Q );
            DiagonalSolve( RIGHT, NORMAL, dCol, Q );
        }
        if( ctrl.primalInit )
        {
            DiagonalScale( LEFT, NORMAL, dCol,  x );
            DiagonalSolve( LEFT, NORMAL, dRowG, s );
        }
        if( ctrl.dualInit )
        {
            DiagonalScale( LEFT, NORMAL, dRowA, y );
            DiagonalScale( LEFT, NORMAL, dRowG, z );
        }
    }
    else
    {
        Ones( dRowA, m, 1 );
        Ones( dRowG, k, 1 );
        Ones( dCol,  n, 1 );
    }

    const Real bNrm2 = Nrm2( b );
    const Real cNrm2 = Nrm2( c );
    const Real hNrm2 = Nrm2( h );
    if( ctrl.print )
    {
        const Real QNrm1 = HermitianOneNorm( LOWER, Q );
        const Real ANrm1 = OneNorm( A );
        const Real GNrm1 = OneNorm( G );
        Output("|| Q ||_1 = ",QNrm1);
        Output("|| c ||_2 = ",cNrm2);
        Output("|| A ||_1 = ",ANrm1);
        Output("|| b ||_2 = ",bNrm2);
        Output("|| G ||_1 = ",GNrm1);
        Output("|| h ||_2 = ",hNrm2);
    }

    Initialize
    ( Q, A, G, b, c, h, x, y, z, s,
      ctrl.primalInit, ctrl.dualInit, ctrl.standardInitShift );

    Real infeasError = 1;
    Real dimacsError = 1, dimacsErrorOld = 1;
    Matrix<Real> J, d,
                 rmu,   rc,    rb,    rh,
                 dxAff, dyAff, dzAff, dsAff,
                 dx,    dy,    dz,    ds;
    Matrix<Real> dSub;
    Permutation p;
    Matrix<Real> dxError, dyError, dzError;
    const Int indent = PushIndent();
    for( Int numIts=0; numIts<=ctrl.maxIts; ++numIts )
    {
        // Ensure that s and z are in the cone
        // ===================================
        const Int sNumNonPos = pos_orth::NumOutside( s );
        const Int zNumNonPos = pos_orth::NumOutside( z );
        if( sNumNonPos > 0 || zNumNonPos > 0 )
            LogicError
            (sNumNonPos," entries of s were nonpositive and ",
             zNumNonPos," entries of z were nonpositive");

        // Compute the duality measure
        // ===========================
        const Real dualProd = Dot(s,z);
        const Real mu = dualProd / k;

        // Check for convergence
        // =====================

        // Compute the relative duality gap
        // --------------------------------
        Zeros( d, n, 1 );
        Hemv( LOWER, Real(1), Q, x, Real(0), d );
        const Real xTQx = Dot(x,d);
        const Real primObj =  xTQx/2 + Dot(c,x);
        const Real dualObj = -xTQx/2 - Dot(b,y) - Dot(h,z);
        const Real relObjGap =
          RelativeObjectiveGap( primObj, dualObj, dualProd );
        const Real relCompGap =
          RelativeComplementarityGap( primObj, dualObj, dualProd );
        const Real maxRelGap = Max( relObjGap, relCompGap );

        // || A x - b ||_2 / (1 + || b ||_2)
        // ---------------------------------
        rb = b; rb *= -1;
        Gemv( NORMAL, Real(1), A, x, Real(1), rb );
        const Real rbNrm2 = Nrm2( rb );
        const Real rbConv = rbNrm2 / (1+bNrm2);

        // || Q x + A^T y + G^T z + c ||_2 / (1 + || c ||_2)
        // -------------------------------------------------
        rc = c;
        Hemv( LOWER,     Real(1), Q, x, Real(1), rc );
        Gemv( TRANSPOSE, Real(1), A, y, Real(1), rc );
        Gemv( TRANSPOSE, Real(1), G, z, Real(1), rc );
        const Real rcNrm2 = Nrm2( rc );
        const Real rcConv = rcNrm2 / (1+cNrm2);

        // || G x + s - h ||_2 / (1 + || h ||_2)
        // -------------------------------------
        rh = h; rh *= -1;
        Gemv( NORMAL, Real(1), G, x, Real(1), rh );
        rh += s;
        const Real rhNrm2 = Nrm2( rh );
        const Real rhConv = rhNrm2 / (1+hNrm2);

        // Now check the pieces
        // --------------------
        dimacsErrorOld = dimacsError;
        infeasError = Max(Max(rbConv,rcConv),rhConv);
        dimacsError = Max(infeasError,maxRelGap);
        if( ctrl.print )
        {
            const Real xNrm2 = Nrm2( x );
            const Real yNrm2 = Nrm2( y );
            const Real zNrm2 = Nrm2( z );
            const Real sNrm2 = Nrm2( s );
            Output
            ("iter ",numIts,":\n",Indent(),
             "  ||  x  ||_2 = ",xNrm2,"\n",Indent(),
             "  ||  y  ||_2 = ",yNrm2,"\n",Indent(),
             "  ||  z  ||_2 = ",zNrm2,"\n",Indent(),
             "  ||  s  ||_2 = ",sNrm2,"\n",Indent(),
             "  || r_b ||_2 / (1 + || b ||_2) = ",rbConv,"\n",Indent(),
             "  || r_c ||_2 / (1 + || c ||_2) = ",rcConv,"\n",Indent(),
             "  || r_h ||_2 / (1 + || h ||_2) = ",rhConv,"\n",Indent(),
             "  primal = ",primObj,"\n",Indent(),
             "  dual   = ",dualObj,"\n",Indent(),
             "  relative duality gap = ",maxRelGap);
        }

        const bool metTolerances =
          infeasError <= ctrl.infeasibilityTol &&
          relCompGap <= ctrl.relativeComplementarityGapTol &&
          relObjGap <= ctrl.relativeObjectiveGapTol;
        if( metTolerances )
        {
            if( dimacsError >= ctrl.minDimacsDecreaseRatio*dimacsErrorOld )
            {
                // We have met the tolerances and progress in the last iteration
                // was not significant.
                break;
            }
            else if( numIts == ctrl.maxIts )
            {
                // We have hit the iteration limit but can declare success.
                break;
            }
        }
        else if( numIts == ctrl.maxIts )
        {
            RuntimeError
            ("Maximum number of iterations (",ctrl.maxIts,") exceeded without ",
             "achieving tolerances");
        }

        // Compute the affine search direction
        // ===================================

        // r_mu := s o z
        // -------------
        rmu = z;
        DiagonalScale( LEFT, NORMAL, s, rmu );

        // Construct the full KKT system
        // -----------------------------
        KKT( Q, A, G, s, z, J );
        KKTRHS( rc, rb, rh, rmu, z, d );

        // Compute the proposed step from the KKT system
        // ---------------------------------------------
        try
        {
            LDL( J, dSub, p, false );
            ldl::SolveAfter( J, dSub, p, d, false );
        }
        catch(...)
        {
            if( metTolerances )
            {
                break;
            }
            else
            {
                // TODO(poulson): Increase regularization and continue.
                RuntimeError("Could not achieve tolerances");
            }
        }
        ExpandSolution( m, n, d, rmu, s, z, dxAff, dyAff, dzAff, dsAff );

        if( ctrl.checkResiduals && ctrl.print )
        {
            dxError = rb;
            Gemv( NORMAL, Real(1), A, dxAff, Real(1), dxError );
            const Real dxErrorNrm2 = Nrm2( dxError );

            dyError = rc;
            Hemv( LOWER,     Real(1), Q, dxAff, Real(1), dyError );
            Gemv( TRANSPOSE, Real(1), A, dyAff, Real(1), dyError );
            Gemv( TRANSPOSE, Real(1), G, dzAff, Real(1), dyError );
            const Real dyErrorNrm2 = Nrm2( dyError );

            dzError = rh;
            Gemv( NORMAL, Real(1), G, dxAff, Real(1), dzError );
            dzError += dsAff;
            const Real dzErrorNrm2 = Nrm2( dzError );

            // TODO(poulson): dmuError

            Output
            ("|| dxError ||_2 / (1 + || r_b ||_2) = ",
             dxErrorNrm2/(1+rbNrm2),"\n",Indent(),
             "|| dyError ||_2 / (1 + || r_c ||_2) = ",
             dyErrorNrm2/(1+rcNrm2),"\n",Indent(),
             "|| dzError ||_2 / (1 + || r_h ||_2) = ",
             dzErrorNrm2/(1+rhNrm2));
        }

        // Compute a centrality parameter
        // ==============================
        Real alphaAffPri = pos_orth::MaxStep( s, dsAff, Real(1) );
        Real alphaAffDual = pos_orth::MaxStep( z, dzAff, Real(1) );
        if( ctrl.forceSameStep )
            alphaAffPri = alphaAffDual = Min(alphaAffPri,alphaAffDual);
        if( ctrl.print )
            Output
            ("alphaAffPri = ",alphaAffPri,", alphaAffDual = ",alphaAffDual);
        // NOTE: dz and ds are used as temporaries
        ds = s;
        dz = z;
        Axpy( alphaAffPri,  dsAff, ds );
        Axpy( alphaAffDual, dzAff, dz );
        const Real muAff = Dot(ds,dz) / degree;
        if( ctrl.print )
            Output("muAff = ",muAff,", mu = ",mu);
        const Real sigma =
          ctrl.centralityRule(mu,muAff,alphaAffPri,alphaAffDual);
        if( ctrl.print )
            Output("sigma=",sigma);

        // Solve for the combined direction
        // ================================
        Shift( rmu, -sigma*mu );
        if( ctrl.mehrotra )
        {
            // r_mu += dsAff o dzAff
            // ---------------------
            // NOTE: dz is used as a temporary
            dz = dzAff;
            DiagonalScale( LEFT, NORMAL, dsAff, dz );
            rmu += dz;
        }

        // Compute the proposed step from the KKT system
        // ---------------------------------------------
        KKTRHS( rc, rb, rh, rmu, z, d );
        ldl::SolveAfter( J, dSub, p, d, false );
        ExpandSolution( m, n, d, rmu, s, z, dx, dy, dz, ds );
        // TODO(poulson): Residual checks

        // Update the current estimates
        // ============================
        Real alphaPri = pos_orth::MaxStep( s, ds, 1/ctrl.maxStepRatio );
        Real alphaDual = pos_orth::MaxStep( z, dz, 1/ctrl.maxStepRatio );
        alphaPri = Min(ctrl.maxStepRatio*alphaPri,Real(1));
        alphaDual = Min(ctrl.maxStepRatio*alphaDual,Real(1));
        if( ctrl.forceSameStep )
            alphaPri = alphaDual = Min(alphaPri,alphaDual);
        if( ctrl.print )
            Output("alphaPri = ",alphaPri,", alphaDual = ",alphaDual);
        Axpy( alphaPri,  dx, x );
        Axpy( alphaPri,  ds, s );
        Axpy( alphaDual, dy, y );
        Axpy( alphaDual, dz, z );
        if( alphaPri == Real(0) && alphaDual == Real(0) )
        {
            if( metTolerances )
            {
                break;
            }
            else
            {
                // TODO(poulson): Increase regularization and continue.
                RuntimeError("Could not achieve tolerances");
            }
        }
    }
    SetIndent( indent );

    if( ctrl.outerEquil )
    {
        DiagonalSolve( LEFT, NORMAL, dCol,  x );
        DiagonalSolve( LEFT, NORMAL, dRowA, y );
        DiagonalSolve( LEFT, NORMAL, dRowG, z );
        DiagonalScale( LEFT, NORMAL, dRowG, s );
    }
}

template<typename Real>
void IPM
( const AbstractDistMatrix<Real>& QPre,
  const AbstractDistMatrix<Real>& APre,
  const AbstractDistMatrix<Real>& GPre,
  const AbstractDistMatrix<Real>& bPre,
  const AbstractDistMatrix<Real>& cPre,
  const AbstractDistMatrix<Real>& hPre,
        AbstractDistMatrix<Real>& xPre,
        AbstractDistMatrix<Real>& yPre,
        AbstractDistMatrix<Real>& zPre,
        AbstractDistMatrix<Real>& sPre,
  const IPMCtrl<Real>& ctrl )
{
    EL_DEBUG_CSE
    const Grid& grid = APre.Grid();
    const int commRank = grid.Rank();
    Timer timer;

    // Ensure that the inputs have the appropriate read/write properties
    DistMatrix<Real> Q(grid), A(grid), G(grid), b(grid), c(grid), h(grid);
    Q.Align(0,0);
    A.Align(0,0);
    G.Align(0,0);
    b.Align(0,0);
    c.Align(0,0);
    Q = QPre;
    A = APre;
    G = GPre;
    b = bPre;
    c = cPre;
    h = hPre;

    ElementalProxyCtrl control;
    control.colConstrain = true;
    control.rowConstrain = true;
    control.colAlign = 0;
    control.rowAlign = 0;

    // NOTE: {x,s} do not need to be a read proxy when !ctrl.primalInit
    DistMatrixReadWriteProxy<Real,Real,MC,MR>
      xProx( xPre, control ),
      sProx( sPre, control ),
    // NOTE: {y,z} do not need to be read proxies when !ctrl.dualInit
      yProx( yPre, control ),
      zProx( zPre, control );
    auto& x = xProx.Get();
    auto& s = sProx.Get();
    auto& y = yProx.Get();
    auto& z = zProx.Get();

    // Equilibrate the QP by diagonally scaling [A;G]
    const Int m = A.Height();
    const Int k = G.Height();
    const Int n = A.Width();
    const Int degree = k;
    DistMatrix<Real,MC,STAR> dRowA(grid),
                             dRowG(grid);
    DistMatrix<Real,MR,STAR> dCol(grid);
    if( ctrl.outerEquil )
    {
        if( ctrl.time && commRank == 0 )
            timer.Start();
        StackedRuizEquil( A, G, dRowA, dRowG, dCol, ctrl.print );
        if( ctrl.time && commRank == 0 )
            Output("RuizEquil: ",timer.Stop()," secs");
        DiagonalSolve( LEFT, NORMAL, dRowA, b );
        DiagonalSolve( LEFT, NORMAL, dRowG, h );
        DiagonalSolve( LEFT, NORMAL, dCol,  c );
        // TODO(poulson): Replace with SymmetricDiagonalSolve
        {
            DiagonalSolve( LEFT, NORMAL, dCol,  Q );
            DiagonalSolve( RIGHT, NORMAL, dCol, Q );
        }
        if( ctrl.primalInit )
        {
            DiagonalScale( LEFT, NORMAL, dCol,  x );
            DiagonalSolve( LEFT, NORMAL, dRowG, s );
        }
        if( ctrl.dualInit )
        {
            DiagonalScale( LEFT, NORMAL, dRowA, y );
            DiagonalScale( LEFT, NORMAL, dRowG, z );
        }
    }
    else
    {
        Ones( dRowA, m, 1 );
        Ones( dRowG, k, 1 );
        Ones( dCol,  n, 1 );
    }

    const Real bNrm2 = Nrm2( b );
    const Real cNrm2 = Nrm2( c );
    const Real hNrm2 = Nrm2( h );
    if( ctrl.print )
    {
        const Real QNrm1 = HermitianOneNorm( LOWER, Q );
        const Real ANrm1 = OneNorm( A );
        const Real GNrm1 = OneNorm( G );
        if( commRank == 0 )
        {
            Output("|| Q ||_1 = ",QNrm1);
            Output("|| c ||_2 = ",cNrm2);
            Output("|| A ||_1 = ",ANrm1);
            Output("|| b ||_2 = ",bNrm2);
            Output("|| G ||_1 = ",GNrm1);
            Output("|| h ||_2 = ",hNrm2);
        }
    }

    if( ctrl.time && commRank == 0 )
        timer.Start();
    Initialize
    ( Q, A, G, b, c, h, x, y, z, s,
      ctrl.primalInit, ctrl.dualInit, ctrl.standardInitShift );
    if( ctrl.time && commRank == 0 )
        Output("Init time: ",timer.Stop()," secs");

    Real infeasError = 1;
    Real dimacsError = 1, dimacsErrorOld = 1;
    DistMatrix<Real> J(grid),     d(grid),
                     rc(grid),    rb(grid),    rh(grid),    rmu(grid),
                     dxAff(grid), dyAff(grid), dzAff(grid), dsAff(grid),
                     dx(grid),    dy(grid),    dz(grid),    ds(grid);
    dsAff.AlignWith( s );
    dzAff.AlignWith( s );
    ds.AlignWith( s );
    dz.AlignWith( s );
    rmu.AlignWith( s );
    DistMatrix<Real> dSub(grid);
    DistPermutation p(grid);
    DistMatrix<Real> dxError(grid), dyError(grid), dzError(grid);
    dzError.AlignWith( s );
    const Int indent = PushIndent();
    for( Int numIts=0; numIts<=ctrl.maxIts; ++numIts )
    {
        // Ensure that s and z are in the cone
        // ===================================
        const Int sNumNonPos = pos_orth::NumOutside( s );
        const Int zNumNonPos = pos_orth::NumOutside( z );
        if( sNumNonPos > 0 || zNumNonPos > 0 )
            LogicError
            (sNumNonPos," entries of s were nonpositive and ",
             zNumNonPos," entries of z were nonpositive");

        // Compute the duality measure
        // ===========================
        const Real dualProd = Dot(s,z);
        const Real mu = dualProd / k;

        // Check for convergence
        // =====================

        // Compute the relative duality gap
        // --------------------------------
        Zeros( d, n, 1 );
        Hemv( LOWER, Real(1), Q, x, Real(0), d );
        const Real xTQx = Dot(x,d);
        const Real primObj =  xTQx/2 + Dot(c,x);
        const Real dualObj = -xTQx/2 - Dot(b,y) - Dot(h,z);
        const Real relObjGap =
          RelativeObjectiveGap( primObj, dualObj, dualProd );
        const Real relCompGap =
          RelativeComplementarityGap( primObj, dualObj, dualProd );
        const Real maxRelGap = Max( relObjGap, relCompGap );

        // || A x - b ||_2 / (1 + || b ||_2) <= tol ?
        // --------------------------------------
        rb = b; rb *= -1;
        Gemv( NORMAL, Real(1), A, x, Real(1), rb );
        const Real rbNrm2 = Nrm2( rb );
        const Real rbConv = rbNrm2 / (1+bNrm2);

        // || Q x + A^T y + G^T z + c ||_2 / (1 + || c ||_2)
        // -------------------------------------------------
        rc = c;
        Hemv( LOWER,     Real(1), Q, x, Real(1), rc );
        Gemv( TRANSPOSE, Real(1), A, y, Real(1), rc );
        Gemv( TRANSPOSE, Real(1), G, z, Real(1), rc );
        const Real rcNrm2 = Nrm2( rc );
        const Real rcConv = rcNrm2 / (1+cNrm2);

        // || G x + s - h ||_2 / (1 + || h ||_2)
        // -------------------------------------
        rh = h; rh *= -1;
        Gemv( NORMAL, Real(1), G, x, Real(1), rh );
        rh += s;
        const Real rhNrm2 = Nrm2( rh );
        const Real rhConv = rhNrm2 / (1+hNrm2);

        // Now check the pieces
        // --------------------
        dimacsErrorOld = dimacsError;
        infeasError = Max(Max(rbConv,rcConv),rhConv);
        dimacsError = Max(infeasError,maxRelGap);
        if( ctrl.print )
        {
            const Real xNrm2 = Nrm2( x );
            const Real yNrm2 = Nrm2( y );
            const Real zNrm2 = Nrm2( z );
            const Real sNrm2 = Nrm2( s );
            if( commRank == 0 )
                Output
                ("iter ",numIts,":\n",Indent(),
                 "  ||  x  ||_2 = ",xNrm2,"\n",Indent(),
                 "  ||  y  ||_2 = ",yNrm2,"\n",Indent(),
                 "  ||  z  ||_2 = ",zNrm2,"\n",Indent(),
                 "  ||  s  ||_2 = ",sNrm2,"\n",Indent(),
                 "  || r_b ||_2 / (1 + || b ||_2) = ",rbConv,"\n",Indent(),
                 "  || r_c ||_2 / (1 + || c ||_2) = ",rcConv,"\n",Indent(),
                 "  || r_h ||_2 / (1 + || h ||_2) = ",rhConv,"\n",Indent(),
                 "  primal = ",primObj,"\n",Indent(),
                 "  dual   = ",dualObj,"\n",Indent(),
                 "  relative duality gap = ",maxRelGap);
        }

        const bool metTolerances =
          infeasError <= ctrl.infeasibilityTol &&
          relCompGap <= ctrl.relativeComplementarityGapTol &&
          relObjGap <= ctrl.relativeObjectiveGapTol;
        if( metTolerances )
        {
            if( dimacsError >= ctrl.minDimacsDecreaseRatio*dimacsErrorOld )
            {
                // We have met the tolerances and progress in the last iteration
                // was not significant.
                break;
            }
            else if( numIts == ctrl.maxIts )
            {
                // We have hit the iteration limit but can declare success.
                break;
            }
        }
        else if( numIts == ctrl.maxIts )
        {
            RuntimeError
            ("Maximum number of iterations (",ctrl.maxIts,") exceeded without ",
             "achieving tolerances");
        }

        // Compute the affine search direction
        // ===================================

        // r_mu := s o z
        // -------------
        rmu = z;
        DiagonalScale( LEFT, NORMAL, s, rmu );

        // Construct the KKT system
        // ------------------------
        KKT( Q, A, G, s, z, J );
        KKTRHS( rc, rb, rh, rmu, z, d );

        // Solve for the direction
        // -----------------------
        try
        {
            if( ctrl.time && commRank == 0 )
                timer.Start();
            LDL( J, dSub, p, false );
            if( ctrl.time && commRank == 0 )
            {
                Output("LDL: ",timer.Stop()," secs");
                timer.Start();
            }
            ldl::SolveAfter( J, dSub, p, d, false );
            if( ctrl.time && commRank == 0 )
                Output("Affine solve: ",timer.Stop()," secs");
        }
        catch(...)
        {
            if( metTolerances )
            {
                break;
            }
            else
            {
                // TODO(poulson): Increase regularization and continue.
                RuntimeError("Could not achieve tolerances");
            }
        }
        ExpandSolution( m, n, d, rmu, s, z, dxAff, dyAff, dzAff, dsAff );

        if( ctrl.checkResiduals && ctrl.print )
        {
            dxError = rb;
            Gemv( NORMAL, Real(1), A, dxAff, Real(1), dxError );
            const Real dxErrorNrm2 = Nrm2( dxError );

            dyError = rc;
            Hemv( LOWER,     Real(1), Q, dxAff, Real(1), dyError );
            Gemv( TRANSPOSE, Real(1), A, dyAff, Real(1), dyError );
            Gemv( TRANSPOSE, Real(1), G, dzAff, Real(1), dyError );
            const Real dyErrorNrm2 = Nrm2( dyError );

            dzError = rh;
            Gemv( NORMAL, Real(1), G, dxAff, Real(1), dzError );
            dzError += dsAff;
            const Real dzErrorNrm2 = Nrm2( dzError );

            // TODO(poulson): dmuError

            if( commRank == 0 )
                Output
                ("|| dxError ||_2 / (1 + || r_b ||_2) = ",
                 dxErrorNrm2/(1+rbNrm2),"\n",Indent(),
                 "|| dyError ||_2 / (1 + || r_c ||_2) = ",
                 dyErrorNrm2/(1+rcNrm2),"\n",Indent(),
                 "|| dzError ||_2 / (1 + || r_h ||_2) = ",
                 dzErrorNrm2/(1+rhNrm2));
        }

        // Compute a centrality parameter
        // ==============================
        Real alphaAffPri = pos_orth::MaxStep( s, dsAff, Real(1) );
        Real alphaAffDual = pos_orth::MaxStep( z, dzAff, Real(1) );
        if( ctrl.forceSameStep )
            alphaAffPri = alphaAffDual = Min(alphaAffPri,alphaAffDual);
        if( ctrl.print && commRank == 0 )
            Output
            ("alphaAffPri = ",alphaAffPri,", alphaAffDual = ",alphaAffDual);
        // NOTE: dz and ds are used as temporaries
        ds = s;
        dz = z;
        Axpy( alphaAffPri,  dsAff, ds );
        Axpy( alphaAffDual, dzAff, dz );
        const Real muAff = Dot(ds,dz) / degree;
        if( ctrl.print && commRank == 0 )
            Output("muAff = ",muAff,", mu = ",mu);
        const Real sigma =
          ctrl.centralityRule(mu,muAff,alphaAffPri,alphaAffDual);
        if( ctrl.print && commRank == 0 )
            Output("sigma=",sigma);

        // Solve for the combined direction
        // ================================
        Shift( rmu, -sigma*mu );
        if( ctrl.mehrotra )
        {
            // r_mu += dsAff o dzAff
            // ---------------------
            // NOTE: dz is used as a temporary
            dz = dzAff;
            DiagonalScale( LEFT, NORMAL, dsAff, dz );
            rmu += dz;
        }

        // Form the new KKT RHS
        // --------------------
        KKTRHS( rc, rb, rh, rmu, z, d );
        // Solve for the new direction
        // ---------------------------
        try
        {
            if( ctrl.time && commRank == 0 )
                timer.Start();
            ldl::SolveAfter( J, dSub, p, d, false );
            if( ctrl.time && commRank == 0 )
                Output("Combined solve: ",timer.Stop()," secs");
        }
        catch(...)
        {
            if( metTolerances )
            {
                break;
            }
            else
            {
                // TODO(poulson): Increase regularization and continue.
                RuntimeError("Could not achieve tolerances");
            }
        }
        ExpandSolution( m, n, d, rmu, s, z, dx, dy, dz, ds );
        // TODO(poulson): Residual checks

        // Update the current estimates
        // ============================
        Real alphaPri = pos_orth::MaxStep( s, ds, 1/ctrl.maxStepRatio );
        Real alphaDual = pos_orth::MaxStep( z, dz, 1/ctrl.maxStepRatio );
        alphaPri = Min(ctrl.maxStepRatio*alphaPri,Real(1));
        alphaDual = Min(ctrl.maxStepRatio*alphaDual,Real(1));
        if( ctrl.forceSameStep )
            alphaPri = alphaDual = Min(alphaPri,alphaDual);
        if( ctrl.print && commRank == 0 )
            Output("alphaPri = ",alphaPri,", alphaDual = ",alphaDual);
        Axpy( alphaPri,  dx, x );
        Axpy( alphaPri,  ds, s );
        Axpy( alphaDual, dy, y );
        Axpy( alphaDual, dz, z );
        if( alphaPri == Real(0) && alphaDual == Real(0) )
        {
            if( metTolerances )
            {
                break;
            }
            else
            {
                // TODO(poulson): Increase regularization and continue.
                RuntimeError("Could not achieve tolerances");
            }
        }
    }
    SetIndent( indent );

    if( ctrl.outerEquil )
    {
        DiagonalSolve( LEFT, NORMAL, dCol,  x );
        DiagonalSolve( LEFT, NORMAL, dRowA, y );
        DiagonalSolve( LEFT, NORMAL, dRowG, z );
        DiagonalScale( LEFT, NORMAL, dRowG, s );
    }
}

template<typename Real>
void IPM
( const SparseMatrix<Real>& QPre,
  const SparseMatrix<Real>& APre,
  const SparseMatrix<Real>& GPre,
  const Matrix<Real>& bPre,
  const Matrix<Real>& cPre,
  const Matrix<Real>& hPre,
        Matrix<Real>& x,
        Matrix<Real>& y,
        Matrix<Real>& z,
        Matrix<Real>& s,
  const IPMCtrl<Real>& ctrl )
{
    EL_DEBUG_CSE

    // Equilibrate the QP by diagonally scaling [A;G]
    auto Q = QPre;
    auto A = APre;
    auto G = GPre;
    auto b = bPre;
    auto c = cPre;
    auto h = hPre;
    const Int m = A.Height();
    const Int k = G.Height();
    const Int n = A.Width();
    const Int degree = k;
    Matrix<Real> dRowA, dRowG, dCol;
    if( ctrl.outerEquil )
    {
        StackedRuizEquil( A, G, dRowA, dRowG, dCol, ctrl.print );
        DiagonalSolve( LEFT, NORMAL, dRowA, b );
        DiagonalSolve( LEFT, NORMAL, dRowG, h );
        DiagonalSolve( LEFT, NORMAL, dCol,  c );
        // TODO(poulson): Replace with SymmetricDiagonalSolve
        {
            DiagonalSolve( LEFT, NORMAL, dCol, Q );
            DiagonalSolve( RIGHT, NORMAL, dCol, Q );
        }
        if( ctrl.primalInit )
        {
            DiagonalScale( LEFT, NORMAL, dCol,  x );
            DiagonalSolve( LEFT, NORMAL, dRowG, s );
        }
        if( ctrl.dualInit )
        {
            DiagonalScale( LEFT, NORMAL, dRowA, y );
            DiagonalScale( LEFT, NORMAL, dRowG, z );
        }
    }
    else
    {
        Ones( dRowA, m, 1 );
        Ones( dRowG, k, 1 );
        Ones( dCol,  n, 1 );
    }

    const Real bNrm2 = Nrm2( b );
    const Real cNrm2 = Nrm2( c );
    const Real hNrm2 = Nrm2( h );
    const Real twoNormEstQ =
      HermitianTwoNormEstimate( Q, ctrl.twoNormKrylovBasisSize );
    const Real twoNormEstA = TwoNormEstimate( A, ctrl.twoNormKrylovBasisSize );
    const Real twoNormEstG = TwoNormEstimate( G, ctrl.twoNormKrylovBasisSize );
    const Real origTwoNormEst = twoNormEstA + twoNormEstG + twoNormEstQ + 1;
    if( ctrl.print )
    {
        Output("|| Q ||_2 estimate: ",twoNormEstQ);
        Output("|| c ||_2 = ",cNrm2);
        Output("|| A ||_2 estimate: ",twoNormEstA);
        Output("|| b ||_2 = ",bNrm2);
        Output("|| G ||_2 estimate: ",twoNormEstG);
        Output("|| h ||_2 = ",hNrm2);
    }

    // TODO(poulson): Expose regularization rules to user
    Matrix<Real> regLarge;
    regLarge.Resize( n+m+k, 1 );
    for( Int i=0; i<n+m+k; ++i )
    {
        if( i < n )        regLarge(i) =  ctrl.xRegLarge;
        else if( i < n+m ) regLarge(i) = -ctrl.yRegLarge;
        else               regLarge(i) = -ctrl.zRegLarge;
    }
    regLarge *= origTwoNormEst;

    // Initialize the static portion of the KKT system
    // ===============================================
    SparseMatrix<Real> JStatic;
    StaticKKT
    ( Q, A, G, Sqrt(ctrl.xRegSmall), Sqrt(ctrl.yRegSmall), Sqrt(ctrl.zRegSmall),
      JStatic, false );

    SparseLDLFactorization<Real> sparseLDLFact;

    Initialize
    ( JStatic, regLarge, b, c, h, x, y, z, s,
      sparseLDLFact,
      ctrl.primalInit, ctrl.dualInit, ctrl.standardInitShift, ctrl.solveCtrl );

    SparseMatrix<Real> J, JOrig;
    Matrix<Real> d,
                 w,
                 rc,    rb,    rh,    rmu,
                 dxAff, dyAff, dzAff, dsAff,
                 dx,    dy,    dz,    ds;

    Real infeasError = 1;
    Real dimacsError = 1, dimacsErrorOld = 1;
    Matrix<Real> dInner;
    Matrix<Real> dxError, dyError, dzError;
    const Int indent = PushIndent();
    for( Int numIts=0; numIts<=ctrl.maxIts; ++numIts )
    {
        // Ensure that s and z are in the cone
        // ===================================
        const Int sNumNonPos = pos_orth::NumOutside( s );
        const Int zNumNonPos = pos_orth::NumOutside( z );
        if( sNumNonPos > 0 || zNumNonPos > 0 )
            LogicError
            (sNumNonPos," entries of s were nonpositive and ",
             zNumNonPos," entries of z were nonpositive");

        // Compute the duality measure and scaling point
        // =============================================
        const Real dualProd = Dot(s,z);
        const Real mu = dualProd / k;
        pos_orth::NesterovTodd( s, z, w );
        //const Real wMaxNorm = MaxNorm( w );

        // Check for convergence
        // =====================

        // Compute relative duality gap
        // ----------------------------
        Zeros( d, n, 1 );
        // NOTE: The following assumes that Q is explicitly symmetric
        Multiply( NORMAL, Real(1), Q, x, Real(0), d );
        const Real xTQx = Dot(x,d);
        const Real primObj =  xTQx/2 + Dot(c,x);
        const Real dualObj = -xTQx/2 - Dot(b,y) - Dot(h,z);
        const Real relObjGap =
          RelativeObjectiveGap( primObj, dualObj, dualProd );
        const Real relCompGap =
          RelativeComplementarityGap( primObj, dualObj, dualProd );
        const Real maxRelGap = Max( relObjGap, relCompGap );

        // || A x - b ||_2 / (1 + || b ||_2)
        // ---------------------------------
        rb = b; rb *= -1;
        Multiply( NORMAL, Real(1), A, x, Real(1), rb );
        const Real rbNrm2 = Nrm2( rb );
        const Real rbConv = rbNrm2 / (1+bNrm2);

        // || Q x + A^T y + G^T z + c ||_2 / (1 + || c ||_2)
        // -------------------------------------------------
        rc = c;
        Multiply( NORMAL,    Real(1), Q, x, Real(1), rc );
        Multiply( TRANSPOSE, Real(1), A, y, Real(1), rc );
        Multiply( TRANSPOSE, Real(1), G, z, Real(1), rc );
        const Real rcNrm2 = Nrm2( rc );
        const Real rcConv = rcNrm2 / (1+cNrm2);

        // || G x + s - h ||_2 / (1 + || h ||_2)
        // -------------------------------------
        rh = h; rh *= -1;
        Multiply( NORMAL, Real(1), G, x, Real(1), rh );
        rh += s;
        const Real rhNrm2 = Nrm2( rh );
        const Real rhConv = rhNrm2 / (1+hNrm2);

        // Now check the pieces
        // --------------------
        dimacsErrorOld = dimacsError;
        infeasError = Max(Max(rbConv,rcConv),rhConv);
        dimacsError = Max(infeasError,maxRelGap);
        if( ctrl.print )
        {
            const Real xNrm2 = Nrm2( x );
            const Real yNrm2 = Nrm2( y );
            const Real zNrm2 = Nrm2( z );
            const Real sNrm2 = Nrm2( s );
            Output
            ("iter ",numIts,":\n",Indent(),
             "  ||  x  ||_2 = ",xNrm2,"\n",Indent(),
             "  ||  y  ||_2 = ",yNrm2,"\n",Indent(),
             "  ||  z  ||_2 = ",zNrm2,"\n",Indent(),
             "  ||  s  ||_2 = ",sNrm2,"\n",Indent(),
             "  || primalInfeas ||_2 / (1 + || b ||_2) = ",rbConv,"\n",Indent(),
             "  || dualInfeas   ||_2 / (1 + || c ||_2) = ",rcConv,"\n",Indent(),
             "  || conicInfeas  ||_2 / (1 + || h ||_2) = ",rhConv,"\n",Indent(),
             "  primal = ",primObj,"\n",Indent(),
             "  dual   = ",dualObj,"\n",Indent(),
             "  relative duality gap = ",maxRelGap);
        }

        const bool metTolerances =
          infeasError <= ctrl.infeasibilityTol &&
          relCompGap <= ctrl.relativeComplementarityGapTol &&
          relObjGap <= ctrl.relativeObjectiveGapTol;
        if( metTolerances )
        {
            if( dimacsError >= ctrl.minDimacsDecreaseRatio*dimacsErrorOld )
            {
                // We have met the tolerances and progress in the last iteration
                // was not significant.
                break;
            }
            else if( numIts == ctrl.maxIts )
            {
                // We have hit the iteration limit but can declare success.
                break;
            }
        }
        else if( numIts == ctrl.maxIts )
        {
            RuntimeError
            ("Maximum number of iterations (",ctrl.maxIts,") exceeded without ",
             "achieving tolerances");
        }

        // Compute the affine search direction
        // ===================================

        // r_mu := s o z
        // -------------
        rmu = z;
        DiagonalScale( LEFT, NORMAL, s, rmu );

        // Construct the KKT system
        // ------------------------
        JOrig = JStatic;
        JOrig.FreezeSparsity();
        FinishKKT( m, n, s, z, JOrig );
        KKTRHS( rc, rb, rh, rmu, z, d );

        // Solve for the direction
        // -----------------------
        J = JOrig;
        J.FreezeSparsity();
        UpdateDiagonal( J, Real(1), regLarge );

        /*
        if( wMaxNorm >= ctrl.ruizEquilTol )
            SymmetricRuizEquil( J, dInner, ctrl.ruizMaxIter, ctrl.print );
        else if( wMaxNorm >= ctrl.diagEquilTol )
            SymmetricDiagonalEquil( J, dInner, ctrl.print );
        else
            Ones( dInner, n+m+k, 1 );
        */
        Ones( dInner, n+m+k, 1 );

        if( numIts == 0 && ctrl.primalInit && ctrl.dualInit )
        {
            const bool hermitian = true;
            const BisectCtrl bisectCtrl;
            sparseLDLFact.Initialize( J, hermitian, bisectCtrl );
        }
        else
        {
            sparseLDLFact.ChangeNonzeroValues( J );
        }

        sparseLDLFact.Factor();

        RegSolveInfo<Real> solveInfo;
        if( ctrl.twoStage )
        {
            solveInfo = reg_ldl::SolveAfter
            ( JOrig, regLarge, dInner, sparseLDLFact, d, ctrl.solveCtrl );
        }
        if( !solveInfo.metRequestedTol )
        {
            solveInfo = reg_ldl::RegularizedSolveAfter
            ( JOrig, regLarge, dInner, sparseLDLFact, d,
              ctrl.solveCtrl.relTol,
              ctrl.solveCtrl.maxRefineIts,
              ctrl.solveCtrl.progress );
            if( !solveInfo.metRequestedTol )
            {
                if( metTolerances )
                {
                    break;
                }
                else
                {
                    // TODO(poulson): Increase regularization and continue.
                    RuntimeError("Could not achieve tolerances");
                }
            }
        }
        ExpandSolution( m, n, d, rmu, s, z, dxAff, dyAff, dzAff, dsAff );

        if( ctrl.checkResiduals && ctrl.print )
        {
            dxError = rb;
            Multiply( NORMAL, Real(1), A, dxAff, Real(1), dxError );
            const Real dxErrorNrm2 = Nrm2( dxError );

            dyError = rc;
            Multiply( NORMAL,    Real(1), Q, dxAff, Real(1), dyError );
            Multiply( TRANSPOSE, Real(1), A, dyAff, Real(1), dyError );
            Multiply( TRANSPOSE, Real(1), G, dzAff, Real(1), dyError );
            const Real dyErrorNrm2 = Nrm2( dyError );

            dzError = rh;
            Multiply( NORMAL, Real(1), G, dxAff, Real(1), dzError );
            dzError += dsAff;
            const Real dzErrorNrm2 = Nrm2( dzError );

            // TODO(poulson): dmuError
            // TODO(poulson): Also compute and print the residuals with
            // regularization

            Output
            ("|| dxError ||_2 / (1 + || r_b ||_2) = ",
             dxErrorNrm2/(1+rbNrm2),"\n",Indent(),
             "|| dyError ||_2 / (1 + || r_c ||_2) = ",
             dyErrorNrm2/(1+rcNrm2),"\n",Indent(),
             "|| dzError ||_2 / (1 + || r_h ||_2) = ",
             dzErrorNrm2/(1+rhNrm2));
        }

        // Compute a centrality parameter
        // ==============================
        Real alphaAffPri = pos_orth::MaxStep( s, dsAff, Real(1) );
        Real alphaAffDual = pos_orth::MaxStep( z, dzAff, Real(1) );
        if( ctrl.forceSameStep )
            alphaAffPri = alphaAffDual = Min(alphaAffPri,alphaAffDual);
        if( ctrl.print )
            Output
            ("alphaAffPri = ",alphaAffPri,", alphaAffDual = ",alphaAffDual);
        // NOTE: dz and ds are used as temporaries
        ds = s;
        dz = z;
        Axpy( alphaAffPri,  dsAff, ds );
        Axpy( alphaAffDual, dzAff, dz );
        const Real muAff = Dot(ds,dz) / degree;
        if( ctrl.print )
            Output("muAff = ",muAff,", mu = ",mu);
        const Real sigma =
          ctrl.centralityRule(mu,muAff,alphaAffPri,alphaAffDual);
        if( ctrl.print )
            Output("sigma=",sigma);

        // Solve for the combined direction
        // ================================
        Shift( rmu, -sigma*mu );
        if( ctrl.mehrotra )
        {
            // r_mu += dsAff o dzAff
            // ---------------------
            // NOTE: dz is used as a temporary
            dz = dzAff;
            DiagonalScale( LEFT, NORMAL, dsAff, dz );
            rmu += dz;
        }

        // Set up the new KKT RHS
        // ----------------------
        KKTRHS( rc, rb, rh, rmu, z, d );
        // Solve for the new direction
        // ---------------------------
        solveInfo.metRequestedTol = false;
        if( ctrl.twoStage )
        {
            solveInfo = reg_ldl::SolveAfter
            ( JOrig, regLarge, dInner, sparseLDLFact, d, ctrl.solveCtrl );
        }
        if( !solveInfo.metRequestedTol )
        {
            solveInfo = reg_ldl::RegularizedSolveAfter
            ( JOrig, regLarge, dInner, sparseLDLFact, d,
              ctrl.solveCtrl.relTol,
              ctrl.solveCtrl.maxRefineIts,
              ctrl.solveCtrl.progress );
            if( !solveInfo.metRequestedTol )
            {
                if( metTolerances )
                {
                    break;
                }
                else
                {
                    // TODO(poulson): Increase regularization and continue.
                    RuntimeError("Could not achieve tolerances");
                }
            }
        }
        ExpandSolution( m, n, d, rmu, s, z, dx, dy, dz, ds );

        // Update the current estimates
        // ============================
        Real alphaPri = pos_orth::MaxStep( s, ds, 1/ctrl.maxStepRatio );
        Real alphaDual = pos_orth::MaxStep( z, dz, 1/ctrl.maxStepRatio );
        alphaPri = Min(ctrl.maxStepRatio*alphaPri,Real(1));
        alphaDual = Min(ctrl.maxStepRatio*alphaDual,Real(1));
        if( ctrl.forceSameStep )
            alphaPri = alphaDual = Min(alphaPri,alphaDual);
        if( ctrl.print )
            Output("alphaPri = ",alphaPri,", alphaDual = ",alphaDual);
        Axpy( alphaPri,  dx, x );
        Axpy( alphaPri,  ds, s );
        Axpy( alphaDual, dy, y );
        Axpy( alphaDual, dz, z );
        if( alphaPri == Real(0) && alphaDual == Real(0) )
        {
            if( metTolerances )
            {
                break;
            }
            else
            {
                // TODO(poulson): Increase regularization and continue.
                RuntimeError("Could not achieve tolerances");
            }
        }
    }
    SetIndent( indent );

    if( ctrl.outerEquil )
    {
        DiagonalSolve( LEFT, NORMAL, dCol,  x );
        DiagonalSolve( LEFT, NORMAL, dRowA, y );
        DiagonalSolve( LEFT, NORMAL, dRowG, z );
        DiagonalScale( LEFT, NORMAL, dRowG, s );
    }
}

template<typename Real>
void IPM
( const DistSparseMatrix<Real>& QPre,
  const DistSparseMatrix<Real>& APre,
  const DistSparseMatrix<Real>& GPre,
  const DistMultiVec<Real>& bPre,
  const DistMultiVec<Real>& cPre,
  const DistMultiVec<Real>& hPre,
        DistMultiVec<Real>& x,
        DistMultiVec<Real>& y,
        DistMultiVec<Real>& z,
        DistMultiVec<Real>& s,
  const IPMCtrl<Real>& ctrl )
{
    EL_DEBUG_CSE

    //const Real selInvTol = Pow(eps,Real(-0.25));
    const Real selInvTol = 0;

    const Grid& grid = APre.Grid();
    const int commRank = grid.Rank();
    Timer timer, iterTimer;

    // Equilibrate the QP by diagonally scaling [A;G]
    auto Q = QPre;
    auto A = APre;
    auto G = GPre;
    auto b = bPre;
    auto h = hPre;
    auto c = cPre;
    const Int m = A.Height();
    const Int k = G.Height();
    const Int n = A.Width();
    const Int degree = k;
    DistMultiVec<Real> dRowA(grid), dRowG(grid), dCol(grid);
    if( ctrl.outerEquil )
    {
        if( commRank == 0 && ctrl.time )
            timer.Start();
        StackedRuizEquil( A, G, dRowA, dRowG, dCol, ctrl.print );
        if( commRank == 0 && ctrl.time )
            Output("RuizEquil: ",timer.Stop()," secs");

        DiagonalSolve( LEFT, NORMAL, dRowA, b );
        DiagonalSolve( LEFT, NORMAL, dRowG, h );
        DiagonalSolve( LEFT, NORMAL, dCol,  c );
        // TODO(poulson): Replace with SymmetricDiagonalSolve
        {
            DiagonalSolve( LEFT, NORMAL, dCol, Q );
            DiagonalSolve( RIGHT, NORMAL, dCol, Q );
        }
        if( ctrl.primalInit )
        {
            DiagonalScale( LEFT, NORMAL, dCol,  x );
            DiagonalSolve( LEFT, NORMAL, dRowG, s );
        }
        if( ctrl.dualInit )
        {
            DiagonalScale( LEFT, NORMAL, dRowA, y );
            DiagonalScale( LEFT, NORMAL, dRowG, z );
        }
    }
    else
    {
        Ones( dRowA, m, 1 );
        Ones( dRowG, k, 1 );
        Ones( dCol,  n, 1 );
    }

    const Real bNrm2 = Nrm2( b );
    const Real cNrm2 = Nrm2( c );
    const Real hNrm2 = Nrm2( h );
    const Real twoNormEstQ =
      HermitianTwoNormEstimate( Q, ctrl.twoNormKrylovBasisSize );
    const Real twoNormEstA = TwoNormEstimate( A, ctrl.twoNormKrylovBasisSize );
    const Real twoNormEstG = TwoNormEstimate( G, ctrl.twoNormKrylovBasisSize );
    const Real origTwoNormEst = twoNormEstA + twoNormEstG + twoNormEstQ + 1;
    if( ctrl.print )
    {
        const double imbalanceQ = Q.Imbalance();
        const double imbalanceA = A.Imbalance();
        const double imbalanceG = G.Imbalance();
        if( commRank == 0 )
        {
            Output("|| Q ||_2 estimate: ",twoNormEstQ);
            Output("|| c ||_2 = ",cNrm2);
            Output("|| A ||_2 estimate: ",twoNormEstA);
            Output("|| b ||_2 = ",bNrm2);
            Output("|| G ||_2 estimate: ",twoNormEstG);
            Output("|| h ||_2 = ",hNrm2);
            Output("Imbalance factor of Q: ",imbalanceQ);
            Output("Imbalance factor of A: ",imbalanceA);
            Output("Imbalance factor of G: ",imbalanceG);
        }
    }

    DistMultiVec<Real> regLarge(grid);
    regLarge.Resize( n+m+k, 1 );
    for( Int iLoc=0; iLoc<regLarge.LocalHeight(); ++iLoc )
    {
        const Int i = regLarge.GlobalRow(iLoc);
        if( i < n )
          regLarge.SetLocal( iLoc, 0,  ctrl.xRegLarge );
        else if( i < n+m )
          regLarge.SetLocal( iLoc, 0, -ctrl.yRegLarge );
        else
          regLarge.SetLocal( iLoc, 0, -ctrl.zRegLarge );
    }
    regLarge *= origTwoNormEst;

    // Compute the static portion of the KKT system
    // ============================================
    DistSparseMatrix<Real> JStatic(grid);
    StaticKKT
    ( Q, A, G, Sqrt(ctrl.xRegSmall), Sqrt(ctrl.yRegSmall), Sqrt(ctrl.zRegSmall),
      JStatic, false );
    JStatic.InitializeMultMeta();
    if( ctrl.print )
    {
        const double imbalanceJ = JStatic.Imbalance();
        if( commRank == 0 )
            Output("Imbalance factor of J: ",imbalanceJ);
    }

    if( commRank == 0 && ctrl.time )
        timer.Start();
    DistSparseLDLFactorization<Real> sparseLDLFact;
    Initialize
    ( JStatic, regLarge, b, c, h, x, y, z, s,
      sparseLDLFact,
      ctrl.primalInit, ctrl.dualInit, ctrl.standardInitShift, ctrl.solveCtrl );
    if( commRank == 0 && ctrl.time )
        Output("Init: ",timer.Stop()," secs");

    DistSparseMatrix<Real> J(grid), JOrig(grid);
    DistMultiVec<Real> d(grid), w(grid),
                       rc(grid),    rb(grid),    rh(grid),    rmu(grid),
                       dxAff(grid), dyAff(grid), dzAff(grid), dsAff(grid),
                       dx(grid),    dy(grid),    dz(grid),    ds(grid);

    Real infeasError = 1;
    Real dimacsError = 1, dimacsErrorOld = 1;
    DistMultiVec<Real> dInner(grid);
    DistMultiVec<Real> dxError(grid), dyError(grid), dzError(grid);
    const Int indent = PushIndent();
    for( Int numIts=0; numIts<=ctrl.maxIts; ++numIts )
    {
        if( ctrl.time && commRank == 0 )
            iterTimer.Start();

        // Ensure that s and z are in the cone
        // ===================================
        const Int sNumNonPos = pos_orth::NumOutside( s );
        const Int zNumNonPos = pos_orth::NumOutside( z );
        if( sNumNonPos > 0 || zNumNonPos > 0 )
            LogicError
            (sNumNonPos," entries of s were nonpositive and ",
             zNumNonPos," entries of z were nonpositive");

        // Compute the duality measure and scaling point
        // =============================================
        const Real dualProd = Dot(s,z);
        const Real mu = dualProd / k;
        pos_orth::NesterovTodd( s, z, w );
        const Real wMaxNorm = MaxNorm( w );

        // Check for convergence
        // =====================

        // Check relative duality gap
        // --------------------------
        Zeros( d, n, 1 );
        // NOTE: The following assumes that Q is explicitly symmetric
        Multiply( NORMAL, Real(1), Q, x, Real(0), d );
        const Real xTQx = Dot(x,d);
        const Real primObj =  xTQx/2 + Dot(c,x);
        const Real dualObj = -xTQx/2 - Dot(b,y) - Dot(h,z);
        const Real relObjGap =
          RelativeObjectiveGap( primObj, dualObj, dualProd );
        const Real relCompGap =
          RelativeComplementarityGap( primObj, dualObj, dualProd );
        const Real maxRelGap = Max( relObjGap, relCompGap );

        // || A x - b ||_2 / (1 + || b ||_2)
        // ---------------------------------
        rb = b; rb *= -1;
        Multiply( NORMAL, Real(1), A, x, Real(1), rb );
        const Real rbNrm2 = Nrm2( rb );
        const Real rbConv = rbNrm2 / (1+bNrm2);

        // || Q x + A^T y + G^T z + c ||_2 / (1 + || c ||_2)
        // -------------------------------------------------
        rc = c;
        Multiply( NORMAL,    Real(1), Q, x, Real(1), rc );
        Multiply( TRANSPOSE, Real(1), A, y, Real(1), rc );
        Multiply( TRANSPOSE, Real(1), G, z, Real(1), rc );
        const Real rcNrm2 = Nrm2( rc );
        const Real rcConv = rcNrm2 / (1+cNrm2);

        // || G x + s - h ||_2 / (1 + || h ||_2)
        // -------------------------------------
        rh = h; rh *= -1;
        Multiply( NORMAL, Real(1), G, x, Real(1), rh );
        rh += s;
        const Real rhNrm2 = Nrm2( rh );
        const Real rhConv = rhNrm2 / (1+hNrm2);

        // Now check the pieces
        // --------------------
        dimacsErrorOld = dimacsError;
        infeasError = Max(Max(rbConv,rcConv),rhConv);
        dimacsError = Max(infeasError,maxRelGap);
        if( ctrl.print )
        {
            const Real xNrm2 = Nrm2( x );
            const Real yNrm2 = Nrm2( y );
            const Real zNrm2 = Nrm2( z );
            const Real sNrm2 = Nrm2( s );
            if( commRank == 0 )
                Output
                ("iter ",numIts,":\n",Indent(),
                 "  ||  x  ||_2 = ",xNrm2,"\n",Indent(),
                 "  ||  y  ||_2 = ",yNrm2,"\n",Indent(),
                 "  ||  z  ||_2 = ",zNrm2,"\n",Indent(),
                 "  ||  s  ||_2 = ",sNrm2,"\n",Indent(),
                 "  || r_b ||_2 / (1 + || b ||_2) = ",rbConv,"\n",Indent(),
                 "  || r_c ||_2 / (1 + || c ||_2) = ",rcConv,"\n",Indent(),
                 "  || r_h ||_2 / (1 + || h ||_2) = ",rhConv,"\n",Indent(),
                 "  primal = ",primObj,"\n",Indent(),
                 "  dual   = ",dualObj,"\n",Indent(),
                 "  relative duality gap = ",maxRelGap);
        }

        const bool metTolerances =
          infeasError <= ctrl.infeasibilityTol &&
          relCompGap <= ctrl.relativeComplementarityGapTol &&
          relObjGap <= ctrl.relativeObjectiveGapTol;
        if( metTolerances )
        {
            if( dimacsError >= ctrl.minDimacsDecreaseRatio*dimacsErrorOld )
            {
                // We have met the tolerances and progress in the last iteration
                // was not significant.
                break;
            }
            else if( numIts == ctrl.maxIts )
            {
                // We have hit the iteration limit but can declare success.
                break;
            }
        }
        else if( numIts == ctrl.maxIts )
        {
            RuntimeError
            ("Maximum number of iterations (",ctrl.maxIts,") exceeded without ",
             "achieving tolerances");
        }

        // Compute the affine search direction
        // ===================================

        // r_mu := s o z
        // -------------
        rmu = z;
        DiagonalScale( LEFT, NORMAL, s, rmu );

        // Construct the KKT system
        // ------------------------
        JOrig = JStatic;
        JOrig.FreezeSparsity();
        JOrig.LockedDistGraph().multMeta = JStatic.LockedDistGraph().multMeta;
        FinishKKT( m, n, s, z, JOrig );
        KKTRHS( rc, rb, rh, rmu, z, d );

        // Solve for the direction
        // -----------------------
        J = JOrig;
        J.FreezeSparsity();
        J.LockedDistGraph().multMeta = JStatic.LockedDistGraph().multMeta;
        UpdateDiagonal( J, Real(1), regLarge );

        /*
        if( commRank == 0 && ctrl.time )
            timer.Start();
        if( wMaxNorm >= ctrl.ruizEquilTol )
            SymmetricRuizEquil( J, dInner, ctrl.ruizMaxIter, ctrl.print );
        else if( wMaxNorm >= ctrl.diagEquilTol )
            SymmetricDiagonalEquil( J, dInner, ctrl.print );
        else
            Ones( dInner, n+m+k, 1 );
        if( commRank == 0 && ctrl.time )
            Output("Equilibration: ",timer.Stop()," secs");
        */
        Ones( dInner, n+m+k, 1 );

        if( numIts == 0 && ctrl.primalInit && ctrl.dualInit )
        {
            const bool hermitian = true;
            const BisectCtrl bisectCtrl;
            sparseLDLFact.Initialize( J, hermitian, bisectCtrl );
        }
        else
        {
            sparseLDLFact.ChangeNonzeroValues( J );
        }

        if( commRank == 0 && ctrl.time )
            timer.Start();
        if( wMaxNorm >= selInvTol )
            sparseLDLFact.Factor( LDL_2D );
        else
            sparseLDLFact.Factor( LDL_SELINV_2D );
        if( commRank == 0 && ctrl.time )
            Output("LDL: ",timer.Stop()," secs");

        if( commRank == 0 && ctrl.time )
            timer.Start();
        RegSolveInfo<Real> solveInfo;
        if( ctrl.twoStage )
        {
            solveInfo = reg_ldl::SolveAfter
            ( JOrig, regLarge, dInner, sparseLDLFact, d, ctrl.solveCtrl );
        }
        if( !solveInfo.metRequestedTol )
        {
            solveInfo = reg_ldl::RegularizedSolveAfter
            ( JOrig, regLarge, dInner, sparseLDLFact, d,
              ctrl.solveCtrl.relTol,
              ctrl.solveCtrl.maxRefineIts,
              ctrl.solveCtrl.progress );
            if( !solveInfo.metRequestedTol )
            {
                if( metTolerances )
                {
                    break;
                }
                else
                {
                    // TODO(poulson): Increase regularization and continue.
                    RuntimeError("Could not achieve tolerances");
                }
            }
        }
        if( commRank == 0 && ctrl.time )
            Output("Affine solve: ",timer.Stop()," secs");
        ExpandSolution( m, n, d, rmu, s, z, dxAff, dyAff, dzAff, dsAff );

        if( ctrl.checkResiduals && ctrl.print )
        {
            dxError = rb;
            Multiply( NORMAL, Real(1), A, dxAff, Real(1), dxError );
            const Real dxErrorNrm2 = Nrm2( dxError );

            dyError = rc;
            Multiply( NORMAL,    Real(1), Q, dxAff, Real(1), dyError );
            Multiply( TRANSPOSE, Real(1), A, dyAff, Real(1), dyError );
            Multiply( TRANSPOSE, Real(1), G, dzAff, Real(1), dyError );
            const Real dyErrorNrm2 = Nrm2( dyError );

            dzError = rh;
            Multiply( NORMAL, Real(1), G, dxAff, Real(1), dzError );
            dzError += dsAff;
            const Real dzErrorNrm2 = Nrm2( dzError );

            // TODO(poulson): dmuError
            // TODO(poulson): Also compute and print the residuals with
            // regularization

            if( commRank == 0 )
                Output
                ("|| dxError ||_2 / (1 + || r_b ||_2) = ",
                 dxErrorNrm2/(1+rbNrm2),"\n",Indent(),
                 "|| dyError ||_2 / (1 + || r_c ||_2) = ",
                 dyErrorNrm2/(1+rcNrm2),"\n",Indent(),
                 "|| dzError ||_2 / (1 + || r_h ||_2) = ",
                 dzErrorNrm2/(1+rhNrm2));
        }

        // Compute a centrality parameter using Mehrotra's formula
        // =======================================================
        Real alphaAffPri = pos_orth::MaxStep( s, dsAff, Real(1) );
        Real alphaAffDual = pos_orth::MaxStep( z, dzAff, Real(1) );
        if( ctrl.forceSameStep )
            alphaAffPri = alphaAffDual = Min(alphaAffPri,alphaAffDual);
        if( ctrl.print && commRank == 0 )
            Output
            ("alphaAffPri = ",alphaAffPri,", alphaAffDual = ",alphaAffDual);
        // NOTE: dz and ds are used as temporaries
        ds = s;
        dz = z;
        Axpy( alphaAffPri,  dsAff, ds );
        Axpy( alphaAffDual, dzAff, dz );
        const Real muAff = Dot(ds,dz) / degree;
        if( ctrl.print && commRank == 0 )
            Output("muAff = ",muAff,", mu = ",mu);
        const Real sigma =
          ctrl.centralityRule(mu,muAff,alphaAffPri,alphaAffDual);
        if( ctrl.print && commRank == 0 )
            Output("sigma=",sigma);

        // Solve for the combined direction
        // ================================
        Shift( rmu, -sigma*mu );
        if( ctrl.mehrotra )
        {
            // r_mu += dsAff o dzAff
            // ---------------------
            // NOTE: dz is being used as a temporary
            dz = dzAff;
            DiagonalScale( LEFT, NORMAL, dsAff, dz );
            rmu += dz;
        }

        // Set up the new RHS
        // ------------------
        KKTRHS( rc, rb, rh, rmu, z, d );
        // Compute the new direction
        // -------------------------
        if( commRank == 0 && ctrl.time )
            timer.Start();
        solveInfo.metRequestedTol = false;
        if( ctrl.twoStage )
        {
            solveInfo = reg_ldl::SolveAfter
            ( JOrig, regLarge, dInner, sparseLDLFact, d, ctrl.solveCtrl );
        }
        if( !solveInfo.metRequestedTol )
        {
            solveInfo = reg_ldl::RegularizedSolveAfter
            ( JOrig, regLarge, dInner, sparseLDLFact, d,
              ctrl.solveCtrl.relTol,
              ctrl.solveCtrl.maxRefineIts,
              ctrl.solveCtrl.progress );
            if( !solveInfo.metRequestedTol )
            {
                if( metTolerances )
                {
                    break;
                }
                else
                {
                    // TODO(poulson): Increase regularization and continue.
                    RuntimeError("Could not achieve tolerances");
                }
            }
        }
        if( commRank == 0 && ctrl.time )
            Output("Corrector solver: ",timer.Stop()," secs");
        ExpandSolution( m, n, d, rmu, s, z, dx, dy, dz, ds );

        // Update the current estimates
        // ============================
        Real alphaPri = pos_orth::MaxStep( s, ds, 1/ctrl.maxStepRatio );
        Real alphaDual = pos_orth::MaxStep( z, dz, 1/ctrl.maxStepRatio );
        alphaPri = Min(ctrl.maxStepRatio*alphaPri,Real(1));
        alphaDual = Min(ctrl.maxStepRatio*alphaDual,Real(1));
        if( ctrl.forceSameStep )
            alphaPri = alphaDual = Min(alphaPri,alphaDual);
        if( ctrl.print && commRank == 0 )
            Output("alphaPri = ",alphaPri,", alphaDual = ",alphaDual);
        Axpy( alphaPri,  dx, x );
        Axpy( alphaPri,  ds, s );
        Axpy( alphaDual, dy, y );
        Axpy( alphaDual, dz, z );
        if( ctrl.time && commRank == 0 )
            Output("iteration: ",iterTimer.Stop()," secs");
        if( alphaPri == Real(0) && alphaDual == Real(0) )
        {
            if( metTolerances )
            {
                break;
            }
            else
            {
                // TODO(poulson): Increase regularization and continue.
                RuntimeError("Could not achieve tolerances");
            }
        }
    }
    SetIndent( indent );

    if( ctrl.outerEquil )
    {
        DiagonalSolve( LEFT, NORMAL, dCol,  x );
        DiagonalSolve( LEFT, NORMAL, dRowA, y );
        DiagonalSolve( LEFT, NORMAL, dRowG, z );
        DiagonalScale( LEFT, NORMAL, dRowG, s );
    }
}

#define PROTO(Real) \
  template void IPM \
  ( const Matrix<Real>& Q, \
    const Matrix<Real>& A, \
    const Matrix<Real>& G, \
    const Matrix<Real>& b, \
    const Matrix<Real>& c, \
    const Matrix<Real>& h, \
          Matrix<Real>& x, \
          Matrix<Real>& y, \
          Matrix<Real>& z, \
          Matrix<Real>& s, \
    const IPMCtrl<Real>& ctrl ); \
  template void IPM \
  ( const AbstractDistMatrix<Real>& Q, \
    const AbstractDistMatrix<Real>& A, \
    const AbstractDistMatrix<Real>& G, \
    const AbstractDistMatrix<Real>& b, \
    const AbstractDistMatrix<Real>& c, \
    const AbstractDistMatrix<Real>& h, \
          AbstractDistMatrix<Real>& x, \
          AbstractDistMatrix<Real>& y, \
          AbstractDistMatrix<Real>& z, \
          AbstractDistMatrix<Real>& s, \
    const IPMCtrl<Real>& ctrl ); \
  template void IPM \
  ( const SparseMatrix<Real>& Q, \
    const SparseMatrix<Real>& A, \
    const SparseMatrix<Real>& G, \
    const Matrix<Real>& b, \
    const Matrix<Real>& c, \
    const Matrix<Real>& h, \
          Matrix<Real>& x, \
          Matrix<Real>& y, \
          Matrix<Real>& z, \
          Matrix<Real>& s, \
    const IPMCtrl<Real>& ctrl ); \
  template void IPM \
  ( const DistSparseMatrix<Real>& Q, \
    const DistSparseMatrix<Real>& A, \
    const DistSparseMatrix<Real>& G, \
    const DistMultiVec<Real>& b, \
    const DistMultiVec<Real>& c, \
    const DistMultiVec<Real>& h, \
          DistMultiVec<Real>& x, \
          DistMultiVec<Real>& y, \
          DistMultiVec<Real>& z, \
          DistMultiVec<Real>& s, \
    const IPMCtrl<Real>& ctrl );

#define EL_NO_INT_PROTO
#define EL_NO_COMPLEX_PROTO
#define EL_ENABLE_DOUBLEDOUBLE
#define EL_ENABLE_QUADDOUBLE
#define EL_ENABLE_QUAD
#define EL_ENABLE_BIGFLOAT
#include <El/macros/Instantiate.h>

} // namespace affine
} // namespace qp
} // namespace El