/*
  Operators 

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Author: Ethan Coon (ecoon@lanl.gov)
*/

#include "Epetra_Vector.h"

#include "CompositeVector.hh"
#include "OperatorDefs.hh"
#include "OperatorUtils.hh"
#include "SuperMap.hh"
#include "TreeVector.hh"

namespace Amanzi {
namespace Operators {

/* ******************************************************************
* Convert composite vector to/from super vector.
****************************************************************** */
int CopyCompositeVectorToSuperVector(const SuperMap& smap, const CompositeVector& cv,
                                     Epetra_Vector& sv, int dofnum)
{
  if (cv.HasComponent("face") && smap.HasComponent("face")) {
    const std::vector<int>& face_inds = smap.Indices("face", dofnum);
    const Epetra_MultiVector& data = *cv.ViewComponent("face");
    for (int f = 0; f != data.MyLength(); ++f) sv[face_inds[f]] = data[0][f];
  } 

  if (cv.HasComponent("cell") && smap.HasComponent("cell")) {
    const std::vector<int>& cell_inds = smap.Indices("cell", dofnum);
    const Epetra_MultiVector& data = *cv.ViewComponent("cell");
    for (int c = 0; c != data.MyLength(); ++c) sv[cell_inds[c]] = data[0][c];
  } 

  if (cv.HasComponent("edge") && smap.HasComponent("edge")) {
    const std::vector<int>& edge_inds = smap.Indices("edge", dofnum);
    const Epetra_MultiVector& data = *cv.ViewComponent("edge");
    for (int e = 0; e != data.MyLength(); ++e) sv[edge_inds[e]] = data[0][e];
  } 

  if (cv.HasComponent("node") && smap.HasComponent("node")) {
    const std::vector<int>& node_inds = smap.Indices("node", dofnum);
    const Epetra_MultiVector& data = *cv.ViewComponent("node");
    for (int v = 0; v != data.MyLength(); ++v) sv[node_inds[v]] = data[0][v];
  }
  return 0;
}


/* ******************************************************************
* Copy super vector to composite vector, component-by-component.
****************************************************************** */
int CopySuperVectorToCompositeVector(const SuperMap& smap, const Epetra_Vector& sv,
                                     CompositeVector& cv, int dofnum)
{
  if (cv.HasComponent("face") && smap.HasComponent("face")) {
    const std::vector<int>& face_inds = smap.Indices("face", dofnum);
    Epetra_MultiVector& data = *cv.ViewComponent("face");
    for (int f = 0; f != data.MyLength(); ++f) data[0][f] = sv[face_inds[f]];
  } 

  if (cv.HasComponent("cell") && smap.HasComponent("cell")) {
    const std::vector<int>& cell_inds = smap.Indices("cell", dofnum);
    Epetra_MultiVector& data = *cv.ViewComponent("cell");
    for (int c = 0; c != data.MyLength(); ++c) data[0][c] = sv[cell_inds[c]];
  } 

  if (cv.HasComponent("edge") && smap.HasComponent("edge")) {
    const std::vector<int>& edge_inds = smap.Indices("edge", dofnum);
    Epetra_MultiVector& data = *cv.ViewComponent("edge");
    for (int e = 0; e != data.MyLength(); ++e) data[0][e] = sv[edge_inds[e]];
  } 

  if (cv.HasComponent("node") && smap.HasComponent("node")) {
    const std::vector<int>& node_inds = smap.Indices("node", dofnum);
    Epetra_MultiVector& data = *cv.ViewComponent("node");
    for (int v = 0; v != data.MyLength(); ++v) data[0][v] = sv[node_inds[v]];
  } 
  return 0;
}


/* ******************************************************************
* Add super vector to composite vector, component-by-component.
****************************************************************** */
int AddSuperVectorToCompositeVector(const SuperMap& smap, const Epetra_Vector& sv,
                                    CompositeVector& cv, int dofnum)
{
  if (cv.HasComponent("face") && smap.HasComponent("face")) {
    const std::vector<int>& face_inds = smap.Indices("face", dofnum);
    Epetra_MultiVector& data = *cv.ViewComponent("face");
    for (int f = 0; f != data.MyLength(); ++f) data[0][f] += sv[face_inds[f]];
  } 

  if (cv.HasComponent("cell") && smap.HasComponent("cell")) {
    const std::vector<int>& cell_inds = smap.Indices("cell", dofnum);
    Epetra_MultiVector& data = *cv.ViewComponent("cell");
    for (int c = 0; c != data.MyLength(); ++c) data[0][c] += sv[cell_inds[c]];
  } 

  if (cv.HasComponent("edge") && smap.HasComponent("edge")) {
    const std::vector<int>& edge_inds = smap.Indices("edge", dofnum);
    Epetra_MultiVector& data = *cv.ViewComponent("edge");
    for (int e = 0; e != data.MyLength(); ++e) data[0][e] += sv[edge_inds[e]];
  } 

  if (cv.HasComponent("node") && smap.HasComponent("node")) {
    const std::vector<int>& node_inds = smap.Indices("node", dofnum);
    Epetra_MultiVector& data = *cv.ViewComponent("node");
    for (int v = 0; v != data.MyLength(); ++v) data[0][v] += sv[node_inds[v]];
  } 
  return 0;
}


/* ******************************************************************
* Nonmember: copy TreeVector to/from Super-vector
****************************************************************** */
int CopyTreeVectorToSuperVector(const SuperMap& map, const TreeVector& tv,
                                Epetra_Vector& sv)
{
  ASSERT(tv.Data() == Teuchos::null);
  int ierr(0);
  int my_dof = 0;
  for (TreeVector::const_iterator it = tv.begin();
       it != tv.end(); ++it) {
    ASSERT((*it)->Data() != Teuchos::null);
    ierr |= CopyCompositeVectorToSuperVector(map, *(*it)->Data(), sv, my_dof);
    my_dof++;            
  }
  ASSERT(!ierr);
  return ierr;
}


int CopySuperVectorToTreeVector(const SuperMap& map,const Epetra_Vector& sv,
                                TreeVector& tv)
{
  ASSERT(tv.Data() == Teuchos::null);
  int ierr(0);
  int my_dof = 0;
  for (TreeVector::iterator it = tv.begin();
       it != tv.end(); ++it) {
    ASSERT((*it)->Data() != Teuchos::null);
    ierr |= CopySuperVectorToCompositeVector(map, sv, *(*it)->Data(), my_dof);
    my_dof++;            
  }
  ASSERT(!ierr);
  return ierr;
}


/* ******************************************************************
* Add super vector to tree vector, subvector-by-subvector.
****************************************************************** */
int AddSuperVectorToTreeVector(const SuperMap& map,const Epetra_Vector& sv,
                               TreeVector& tv)
{
  ASSERT(tv.Data() == Teuchos::null);
  int ierr(0);
  int my_dof = 0;
  for (TreeVector::iterator it = tv.begin();
       it != tv.end(); ++it) {
    ASSERT((*it)->Data() != Teuchos::null);
    ierr |= AddSuperVectorToCompositeVector(map, sv, *(*it)->Data(), my_dof);
    my_dof++;            
  }
  ASSERT(!ierr);
  return ierr;
}


/* ******************************************************************
* Create super map.
****************************************************************** */
Teuchos::RCP<SuperMap> CreateSuperMap(const CompositeVectorSpace& cvs, int schema, int n_dofs)
{
  std::vector<std::string> compnames;
  std::vector<int> dofnums;
  std::vector<Teuchos::RCP<const Epetra_Map> > maps;
  std::vector<Teuchos::RCP<const Epetra_Map> > ghost_maps;

  if (schema & OPERATOR_SCHEMA_DOFS_FACE) {
    ASSERT(cvs.HasComponent("face"));
    compnames.push_back("face");
    dofnums.push_back(n_dofs);
    std::pair<Teuchos::RCP<const Epetra_Map>, Teuchos::RCP<const Epetra_Map> > meshmaps =
        getMaps(*cvs.Mesh(), AmanziMesh::FACE);
    maps.push_back(meshmaps.first);
    ghost_maps.push_back(meshmaps.second);
  }

  if (schema & OPERATOR_SCHEMA_DOFS_CELL) {
    ASSERT(cvs.HasComponent("cell"));
    compnames.push_back("cell");
    dofnums.push_back(n_dofs);
    std::pair<Teuchos::RCP<const Epetra_Map>, Teuchos::RCP<const Epetra_Map> > meshmaps =
        getMaps(*cvs.Mesh(), AmanziMesh::CELL);
    maps.push_back(meshmaps.first);
    ghost_maps.push_back(meshmaps.second);
  }

  if (schema & OPERATOR_SCHEMA_DOFS_EDGE) {
    ASSERT(cvs.HasComponent("edge"));
    compnames.push_back("edge");
    dofnums.push_back(n_dofs);
    std::pair<Teuchos::RCP<const Epetra_Map>, Teuchos::RCP<const Epetra_Map> > meshmaps =
        getMaps(*cvs.Mesh(), AmanziMesh::EDGE);
    maps.push_back(meshmaps.first);
    ghost_maps.push_back(meshmaps.second);
  }

  if (schema & OPERATOR_SCHEMA_DOFS_NODE) {
    ASSERT(cvs.HasComponent("node"));
    compnames.push_back("node");
    dofnums.push_back(n_dofs);
    std::pair<Teuchos::RCP<const Epetra_Map>, Teuchos::RCP<const Epetra_Map> > meshmaps =
        getMaps(*cvs.Mesh(), AmanziMesh::NODE);
    maps.push_back(meshmaps.first);
    ghost_maps.push_back(meshmaps.second);
  }

  return Teuchos::rcp(new SuperMap(cvs.Comm(), compnames, dofnums, maps, ghost_maps));
}


/* ******************************************************************
* Estimate size of the matrix graph.
****************************************************************** */
unsigned int MaxRowSize(const AmanziMesh::Mesh& mesh, int schema, unsigned int n_dofs)
{
  unsigned int row_size(0);
  int dim = mesh.space_dimension();
  if (schema & OPERATOR_SCHEMA_DOFS_FACE) {
    unsigned int i = (dim == 2) ? OPERATOR_QUAD_FACES : OPERATOR_HEX_FACES;

    for (int c = 0; c < mesh.num_entities(AmanziMesh::CELL, AmanziMesh::OWNED); ++c) {
      i = std::max(i, mesh.cell_get_num_faces(c));
    }
    row_size += 2 * i;
  }

  if (schema & OPERATOR_SCHEMA_DOFS_CELL) {
    unsigned int i = (dim == 2) ? OPERATOR_QUAD_FACES : OPERATOR_HEX_FACES;
    row_size += i + 1;
  }

  if (schema & OPERATOR_SCHEMA_DOFS_NODE) {
    unsigned int i = (dim == 2) ? OPERATOR_QUAD_NODES : OPERATOR_HEX_NODES;
    row_size += 8 * i;
  }

  return row_size * n_dofs;
}    

}  // namespace Operators
}  // namespace Amanzi
