/*
  This is the operators component of the Amanzi code. 

  Copyright 2010-2012 held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#include <cstdlib>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

// TPLs
#include "Teuchos_RCP.hpp"
#include "Teuchos_ParameterList.hpp"
#include "Teuchos_ParameterXMLFileReader.hpp"
#include "UnitTest++.h"

// Amanzi
#include "MeshFactory.hh"
#include "GMVMesh.hh"
#include "Tensor.hh"

// Operators
#include "HeatConduction.hh"

#include "OperatorDefs.hh"
#include "OperatorDiffusion.hh"
#include "OperatorDiffusionFactory.hh"
#include "UpwindSecondOrder.hh"
#include "UpwindStandard.hh"


/* *****************************************************************
* Comparison of gravity models with constant and vector density.
***************************************************************** */
void RunTestGravity(std::string op_list_name) {
  using namespace Amanzi;
  using namespace Amanzi::AmanziMesh;
  using namespace Amanzi::AmanziGeometry;
  using namespace Amanzi::Operators;

  Epetra_MpiComm comm(MPI_COMM_WORLD);
  int MyPID = comm.MyPID();
  if (MyPID == 0) std::cout << "\nTest: check gravity induced rhs" << std::endl;

  // read parameter list
  std::string xmlFileName = "test/operator_diffusion.xml";
  Teuchos::ParameterXMLFileReader xmlreader(xmlFileName);
  Teuchos::ParameterList plist = xmlreader.getParameters();
  Teuchos::ParameterList op_list = plist.get<Teuchos::ParameterList>("PK operator").sublist(op_list_name);

  // create a mesh framework
  FrameworkPreference pref;
  pref.clear();
  pref.push_back(MSTK);
  pref.push_back(STKMESH);

  MeshFactory meshfactory(&comm);
  meshfactory.preference(pref);
  Teuchos::RCP<const Mesh> mesh = meshfactory(0.0, 0.0, 1.0, 1.0, 3, 3, NULL);

  // create diffusion coefficient
  int ncells = mesh->num_entities(AmanziMesh::CELL, AmanziMesh::OWNED);
  int nfaces_wghost = mesh->num_entities(AmanziMesh::FACE, AmanziMesh::USED);

  const WhetStone::Tensor Kc(2, 1);
  Teuchos::RCP<std::vector<WhetStone::Tensor> > K = Teuchos::rcp(new std::vector<WhetStone::Tensor>());

  for (int c = 0; c < ncells; c++) {
    Kc(0, 0) = 1.0 + fabs((mesh->cell_centroid(c))[0]);
    K->push_back(Kc);
  }

  AmanziGeometry::Point g(0.0, -1.0);

  // create homogeneous boundary data
  std::vector<int> bc_model(nfaces_wghost, Operators::OPERATOR_BC_NONE);
  std::vector<double> bc_value(nfaces_wghost, 0.0), bc_mixed(nfaces_wghost, 0.0);
  Teuchos::RCP<BCs> bc = Teuchos::rcp(new BCs(Operators::OPERATOR_BC_TYPE_FACE, bc_model, bc_value, bc_mixed));

  // create fluid densities
  CompositeVectorSpace cvs;
  cvs.SetMesh(mesh)
    ->SetGhosted(true)
    ->AddComponent("cell", AmanziMesh::CELL, 1)
    ->AddComponent("face", AmanziMesh::FACE, 1);

  double rho(2.0);
  Teuchos::RCP<CompositeVector> rho_cv = Teuchos::rcp(new CompositeVector(cvs));
  rho_cv->PutScalar(2.0);

  // we need flux and dummy solution to populate nonlinear coefficient
  Teuchos::RCP<CompositeVector> flux = Teuchos::rcp(new CompositeVector(cvs));
  Epetra_MultiVector& flx = *flux->ViewComponent("face", true);

  Point velocity(-1.0, 0.0);
  for (int f = 0; f < nfaces_wghost; f++) {
    const Point& normal = mesh->face_normal(f);
    flx[0][f] = velocity * normal;
  }
  CompositeVector u(cvs);
  
  // create nonlinear coefficient.
  Teuchos::RCP<HeatConduction> knc = Teuchos::rcp(new HeatConduction(mesh));

  // create upwind model
  Teuchos::ParameterList& ulist = plist.sublist("PK operator").sublist("upwind");
  UpwindStandard<HeatConduction> upwind(mesh, knc);
  upwind.Init(ulist);

  knc->UpdateValues(*flux);  // argument is not used
  ModelUpwindFn func = &HeatConduction::Conduction;
  upwind.Compute(*flux, u, bc_model, bc_value, *knc->values(), *knc->values(), func);

  // create first diffusion operator using constant density
  Operators::OperatorDiffusionFactory opfactory;
  Teuchos::RCP<OperatorDiffusion> op1 = opfactory.Create(op_list, mesh, bc, rho, g);

  op1->Setup(K, knc->values(), knc->derivatives());
  op1->UpdateMatrices(flux.ptr(), Teuchos::null);

  // create and populate the second operator using vector density
  Teuchos::RCP<OperatorDiffusion> op2 = opfactory.Create(op_list, mesh, bc, rho_cv, g);

  op2->Setup(K, knc->values(), knc->derivatives());
  op2->UpdateMatrices(flux.ptr(), Teuchos::null);

  // check norm of the right-hand sides
  double a1, a2; 
  CompositeVector& rhs1 = *op1->global_operator()->rhs();
  CompositeVector& rhs2 = *op2->global_operator()->rhs();

  rhs1.Norm2(&a1);
  rhs2.Norm2(&a2);

  if (MyPID == 0) {
    std::cout << "||rhs1||=" << a1 << std::endl;
    std::cout << "||rhs2||=" << a2 << std::endl;
  }
  CHECK_CLOSE(a1, a2, 1e-12);
}


/* *****************************************************************
* Two tests fo rMFd and FV methods.
* **************************************************************** */
TEST(OPERATOR_DIFFUSION_GRAVITY_MFD) {
  RunTestGravity("diffusion operator gravity mfd");
}

TEST(OPERATOR_DIFFUSION_GRAVITY_FV) {
  RunTestGravity("diffusion operator gravity fv");
}
