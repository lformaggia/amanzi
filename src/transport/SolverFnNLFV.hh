/*
  This is the transport component of the Amanzi code.

  Authors: Konstantin Lipnikov (lipnikov@lanl.gov)
*/


#ifndef AMANZI_TRANSPORT_SOLVER_FN_NLFV_
#define AMANZI_TRANSPORT_SOLVER_FN_NLFV_

#include <vector>

#include "Teuchos_RCP.hpp"
#include "Teuchos_ParameterList.hpp"

#include "Epetra_Vector.h"

#include "LinearOperatorFactory.hh"
#include "SolverFnBase.hh"
#include "Transport_PK.hh"
#include "Transport_State.hh"

namespace Amanzi {
namespace AmanziTransport {

template<class Vector>
class SolverFnNLFV : public AmanziSolvers::SolverFnBase<Vector> {
 public:
  SolverFnNLFV(Teuchos::RCP<const AmanziMesh::Mesh> mesh, 
               Teuchos::RCP<Transport_PK> TPK, Teuchos::RCP<Vector> b) :
      mesh_(mesh), TPK_(TPK), b_(b) {
    Teuchos::RCP<Transport_State> TS = TPK_->transport_state();
    const Epetra_Vector& phi = TS->ref_porosity();
    const Epetra_Vector& flux = TS->ref_darcy_flux();
    const Epetra_Vector& ws = TS->ref_water_saturation();

    Teuchos::RCP<Matrix_Dispersion> matrix = TPK_->dispersion_matrix();
    matrix->CalculateDispersionTensor(flux, phi, ws);
    matrix->InitNLFV();
    matrix->CreateFluxStencils();
  }

  ~SolverFnNLFV() {};

  // computes the non-linear functional r = F(u)
  void Residual(const Teuchos::RCP<Vector>& u, Teuchos::RCP<Vector>& r);

  // preconditioner toolkit
  void ApplyPreconditioner(const Teuchos::RCP<const Vector>& v,
                           const Teuchos::RCP<Vector>& hv);
  void UpdatePreconditioner(const Teuchos::RCP<const Vector>& u) {
    TPK_->dispersion_matrix()->UpdatePreconditioner();
  }

  // error analysis
  double ErrorNorm(const Teuchos::RCP<const Vector>& u, 
                   const Teuchos::RCP<const Vector>& du);

  // allow PK to modify a correction
  bool ModifyCorrection(const Teuchos::RCP<const Vector>& r, 
                        const Teuchos::RCP<const Vector>& u, 
                        const Teuchos::RCP<Vector>& du);

 private:
  Teuchos::RCP<const AmanziMesh::Mesh> mesh_;
  Teuchos::RCP<Transport_PK> TPK_; 
  Teuchos::RCP<Vector> b_; 
};


/* ******************************************************************
* Nonliner residual in NLFV
****************************************************************** */
template<class Vector>
void SolverFnNLFV<Vector>::Residual(const Teuchos::RCP<Vector>& u, Teuchos::RCP<Vector>& r)
{
  Teuchos::RCP<Matrix_Dispersion> matrix = TPK_->dispersion_matrix();
  Teuchos::RCP<Transport_State> TS = TPK_->transport_state();

  matrix->AssembleGlobalMatrixNLFV(*u);
  matrix->AddTimeDerivative(TPK_->TimeStep(), TS->ref_porosity(), TS->ref_water_saturation());

  matrix->Apply(*u, *r);
  r->Update(1.0, *b_, -1.0);
}


/* ******************************************************************
* Use linear solver. 
****************************************************************** */
template<class Vector>
void SolverFnNLFV<Vector>::ApplyPreconditioner(
    const Teuchos::RCP<const Vector>& v, const Teuchos::RCP<Vector>& hv)
{
  AmanziSolvers::LinearOperatorFactory<Matrix_Dispersion, Epetra_Vector, Epetra_BlockMap> factory;
  Teuchos::RCP<AmanziSolvers::LinearOperator<Matrix_Dispersion, Epetra_Vector, Epetra_BlockMap> >
     solver = factory.Create("Dispersion Solver", TPK_->solvers_list, TPK_->dispersion_matrix());

  solver->ApplyInverse(*v, *hv);
}


/* ******************************************************************
* Calculate residual error.                                                       
****************************************************************** */
template<class Vector>
double SolverFnNLFV<Vector>::ErrorNorm(
    const Teuchos::RCP<const Vector>& u, const Teuchos::RCP<const Vector>& r)
{ 
  double rnorm;
  r->Norm2(&rnorm);
  return rnorm;
}


/* ******************************************************************
* Calculate relaxation factor.                                                       
****************************************************************** */
template<class Vector>
bool SolverFnNLFV<Vector>::ModifyCorrection(
    const Teuchos::RCP<const Vector>& r, 
    const Teuchos::RCP<const Vector>& u, 
    const Teuchos::RCP<Vector>& du)
{ 
  return true;
}

}  // namespace AmanziFlow
}  // namespace Amanzi

#endif
