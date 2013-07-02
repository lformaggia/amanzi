#include <RichardSolver.H>
#include <RICHARDSOLVER_F.H>
#include <POROUSMEDIA_F.H>

#include <Utility.H>


#undef PETSC_3_2
#define PETSC_3_2 1

static bool use_fd_jac_DEF = true;
static bool use_dense_Jacobian_DEF = false;
static bool upwind_krel_DEF = false;
static bool subgrid_krel_DEF = false; // Default off, not debugged yet
static int pressure_maxorder_DEF = 3;
static Real errfd_DEF = 1.e-8;
static int max_ls_iterations_DEF = 10;
static Real min_ls_factor_DEF = 1.e-8;
static Real ls_acceptance_factor_DEF = 1.4;
static Real ls_reduction_factor_DEF = 0.1;
static int monitor_line_search_DEF = 0;
static int  maxit_DEF = 30;
static int maxf_DEF = 1e8;
static Real atol_DEF = 1e-10; 
static Real rtol_DEF = 1e-20;
static Real stol_DEF = 1e-12;
static bool scale_soln_before_solve_DEF = true;
static bool semi_analytic_J_DEF = false; // There is a bug in this, or at least a set of solver configs reqd
static bool centered_diff_J_DEF = true;
static Real variable_switch_saturation_threshold_DEF = -0.9999;
static std::string ls_reason_DEF = "Invalid";
static bool ls_success_DEF = false;

static int max_num_Jacobian_reuses_DEF = 0; // This just doesnt seem to work very well....
static bool dump_Jacobian_and_exit = false;

RSParams::RSParams()
{
  // Set default values for all parameters
  use_fd_jac = use_fd_jac_DEF;
  use_dense_Jacobian = use_dense_Jacobian_DEF;
  upwind_krel = upwind_krel_DEF;
  subgrid_krel = subgrid_krel_DEF;
  pressure_maxorder = pressure_maxorder_DEF;
  errfd = errfd_DEF;
  max_ls_iterations = max_ls_iterations_DEF;
  min_ls_factor = min_ls_factor_DEF;
  ls_acceptance_factor = ls_acceptance_factor_DEF;
  ls_reduction_factor = ls_reduction_factor_DEF;
  monitor_line_search = monitor_line_search_DEF;
  maxit = maxit_DEF;
  maxf = maxf_DEF;
  atol = atol_DEF;
  rtol = rtol_DEF;
  stol = stol_DEF;
  scale_soln_before_solve = scale_soln_before_solve_DEF;
  semi_analytic_J = semi_analytic_J_DEF;
  centered_diff_J = centered_diff_J_DEF;
  variable_switch_saturation_threshold = variable_switch_saturation_threshold_DEF;
  max_num_Jacobian_reuses = max_num_Jacobian_reuses_DEF;
  ls_success = ls_success_DEF;
  ls_reason = ls_reason_DEF;
}

static RichardSolver* static_rs_ptr = 0;

void
RichardSolver::SetTheRichardSolver(RichardSolver* ptr) 
{
    static_rs_ptr = ptr;
}

void 
RichardSolver::SetCurrentTimestep(int step)
{
    current_timestep = step;
}

int 
RichardSolver::GetCurrentTimestep() const
{
    return current_timestep;
}


// Forward declaration of local helper functions
static void MatSqueeze(Mat& J);
PetscErrorCode RichardComputeJacobianColor(SNES snes,Vec x1,Mat *J,Mat *B,MatStructure *flag,void *ctx);
PetscErrorCode RichardMatFDColoringApply(Mat J,MatFDColoring coloring,Vec x1,MatStructure *flag,void *sctx);
PetscErrorCode SemiAnalyticMatFDColoringApply(Mat J,MatFDColoring coloring,Vec x1,MatStructure *flag,void *sctx);
PetscErrorCode PostCheck(SNES snes,Vec x,Vec y,Vec w,void *ctx,PetscBool  *changed_y,PetscBool  *changed_w);
PetscErrorCode PostCheckAlt(SNES snes,Vec x,Vec y,Vec w,void *ctx,PetscBool  *changed_y,PetscBool  *changed_w);
PetscErrorCode RichardJacFromPM(SNES snes, Vec x, Mat* jac, Mat* jacpre, MatStructure* flag, void *dummy);
PetscErrorCode RichardRes_DpDt(SNES snes,Vec x,Vec f,void *dummy);
PetscErrorCode RichardR2(SNES snes,Vec x,Vec f,void *dummy);

struct CheckCtx
{
    RichardSolver* rs;
    RichardNLSdata* nld;
};


#undef __FUNCT__  
#define __FUNCT__ "RichardSolverCtr"
RichardSolver::RichardSolver(PMAmr&          _pm_amr,
			     const RSParams& _params,
			     Layout&         _layout)
  : pm_amr(_pm_amr),
    layout(_layout),
    params(_params),
    KappaCCdir(0)
{
  nLevs = layout.NumLevels();
  mftfp = new MFTFillPatch(layout);
  pm.resize(pm_amr.finestLevel()+1,PArrayNoManage);
  for (int lev = 0; lev < pm.size(); lev++)  {
    pm.set(lev,dynamic_cast<PorousMedia*>(&pm_amr.getLevel(lev)));
  }

  // These will be set prior to each solve call in order to support the case that
  // the unlying multifabs get changed between repeated calls to this solver (AMR
  // typically advances the state data by swapping the underlying pointers).
  RhoSatOld = 0;
  RhoSatNew = 0;
  Pnew = 0;
  KappaCCdir = 0;
  CoeffCC = 0;

  PArray<MultiFab> lambda(nLevs,PArrayNoManage);
  PArray<MultiFab> kappaccavg(nLevs,PArrayNoManage);
  PArray<MultiFab> kappaccdir(nLevs,PArrayNoManage);
  PArray<MultiFab> porosity(nLevs,PArrayNoManage);
  PArray<MultiFab> pcap_params(nLevs,PArrayNoManage);

  for (int lev=0; lev<nLevs; ++lev) {
    lambda.set(lev,pm[lev].LambdaCC_Curr());
    porosity.set(lev,pm[lev].Porosity());
    pcap_params.set(lev,pm[lev].PCapParams());
    if (!params.use_fd_jac || params.semi_analytic_J || params.variable_switch_saturation_threshold) {
        kappaccavg.set(lev,pm[lev].KappaCCavg());
    }
  }

  if (!params.use_fd_jac || params.semi_analytic_J || params.variable_switch_saturation_threshold) {
    KappaCCavg = new MFTower(layout,kappaccavg,nLevs);
  }
  else {
    KappaCCavg = 0;
  }
  Lambda = new MFTower(layout,lambda,nLevs);
  Porosity = new MFTower(layout,porosity,nLevs);
  PCapParams = new MFTower(layout,pcap_params,nLevs);
      
  ctmp.resize(BL_SPACEDIM);
  Rhs = new MFTower(layout,IndexType(IntVect::TheZeroVector()),1,1,nLevs);
  Alpha = new MFTower(layout,IndexType(IntVect::TheZeroVector()),1,1,nLevs);
  
  RichardCoefs.resize(BL_SPACEDIM,PArrayManage);
  DarcyVelocity.resize(BL_SPACEDIM,PArrayManage);
  for (int d=0; d<BL_SPACEDIM; ++d) {
    PArray<MultiFab> utmp(nLevs,PArrayNoManage);
    ctmp[d].resize(nLevs,PArrayManage);
    for (int lev=0; lev<nLevs; ++lev) {
      BoxArray ba = BoxArray(lambda[lev].boxArray()).surroundingNodes(d);
      ctmp[d].set(lev, new MultiFab(ba,1,0));
      utmp.set(lev,&(pm[lev].UMac_Curr()[d]));
    }
    DarcyVelocity.set(d, new MFTower(layout,utmp,nLevs));
    RichardCoefs.set(d, new MFTower(layout,ctmp[d],nLevs));
    utmp.clear();
  }

  if (!params.upwind_krel) {
    CoeffCC = new MFTower(layout,IndexType(IntVect::TheZeroVector()),BL_SPACEDIM,1,nLevs);
  }

  PetscErrorCode ierr;       
  int n = layout.NumberOfLocalNodeIds();
  int N = layout.NumberOfGlobalNodeIds();
  MPI_Comm comm = ParallelDescriptor::Communicator();
  ierr = VecCreateMPI(comm,n,N,&RhsV); CHKPETSC(ierr);
  ierr = VecDuplicate(RhsV,&SolnV); CHKPETSC(ierr);
  ierr = VecDuplicate(RhsV,&SolnTypV); CHKPETSC(ierr);
  ierr = VecDuplicate(RhsV,&SolnTypInvV); CHKPETSC(ierr);
  ierr = VecDuplicate(RhsV,&GV); CHKPETSC(ierr);
  ierr = VecDuplicate(RhsV,&AlphaV); CHKPETSC(ierr);

  const BCRec& pressure_bc = pm[0].get_desc_lst()[Press_Type].getBC(0);
  mftfp->BuildStencil(pressure_bc, params.pressure_maxorder);

  gravity.resize(BL_SPACEDIM,0);
  gravity[BL_SPACEDIM-1] = PorousMedia::getGravity();
  density = PorousMedia::Density();

  // Estmated number of nonzero local columns of J
  int d_nz = (params.use_dense_Jacobian ? N : 1 + (params.pressure_maxorder-1)*(2*BL_SPACEDIM)); 
  int o_nz = 0; // Estimated number of nonzero nonlocal (off-diagonal) columns of J

#if defined(PETSC_3_2)
  ierr = MatCreateMPIAIJ(comm, n, n, N, N, d_nz, PETSC_NULL, o_nz, PETSC_NULL, &Jac); CHKPETSC(ierr);
#else
  ierr = MatCreate(comm, &Jac); CHKPETSC(ierr);
  ierr = MatSetSizes(Jac,n,n,N,N);  CHKPETSC(ierr);
  ierr = MatSetFromOptions(Jac); CHKPETSC(ierr);
  ierr = MatSeqAIJSetPreallocation(Jac, d_nz*d_nz, PETSC_NULL); CHKPETSC(ierr);
  ierr = MatMPIAIJSetPreallocation(Jac, d_nz, PETSC_NULL, o_nz, PETSC_NULL); CHKPETSC(ierr);
#endif  

  BuildOpSkel(Jac);
  BuildMLPropEval();
  
  matfdcoloring = 0;
  ierr = SNESCreate(comm,&snes); CHKPETSC(ierr);
  ierr = SNESSetFunction(snes,RhsV,RichardRes_DpDt,(void*)(this)); CHKPETSC(ierr);

  if (params.use_fd_jac) {
    ierr = MatGetColoring(Jac,MATCOLORINGSL,&iscoloring); CHKPETSC(ierr);
    ierr = MatFDColoringCreate(Jac,iscoloring,&matfdcoloring); CHKPETSC(ierr);
    if (params.semi_analytic_J) {
        ierr = MatFDColoringSetFunction(matfdcoloring,
                                        (PetscErrorCode (*)(void))RichardR2,
                                        (void*)(this)); CHKPETSC(ierr);
    }
    else {
        ierr = MatFDColoringSetFunction(matfdcoloring,
                                        (PetscErrorCode (*)(void))RichardRes_DpDt,
                                        (void*)(this)); CHKPETSC(ierr);
    }
    ierr = MatFDColoringSetFromOptions(matfdcoloring); CHKPETSC(ierr);
    ierr = MatFDColoringSetParameters(matfdcoloring,params.errfd,PETSC_DEFAULT);CHKPETSC(ierr);
    ierr = SNESSetJacobian(snes,Jac,Jac,RichardComputeJacobianColor,matfdcoloring);CHKPETSC(ierr);
  }
  else {
    ierr = SNESSetJacobian(snes,Jac,Jac,RichardJacFromPM,(void*)(this));CHKPETSC(ierr);
  }

  ierr = SNESSetTolerances(snes,params.atol,params.rtol,params.stol,params.maxit,params.maxf);CHKPETSC(ierr);
  ierr = SNESSetFromOptions(snes);CHKPETSC(ierr);
}

BoxArray 
ComplementIn(const BoxArray& ba, const BoxArray& ba_in, bool do_simplify = false)
{
  BoxList bl;
  for (int i=0; i<ba_in.size(); ++i) {
    bl.join(BoxList(BoxLib::complementIn(ba_in[i],ba)));
  }
  if (do_simplify) {
    bl.simplify();
  }
  return BoxArray(bl);
}

BoxArray 
Join(const BoxArray& ba1, const BoxArray& ba2, bool do_simplify = false)
{
  BoxList bl(ba1);
  bl.join(ba2.boxList());
  if (do_simplify) {
    bl.simplify();
  }
  return BoxArray(bl);
}

void
RichardSolver::BuildMLPropEval()
{
  if (!params.upwind_krel  &&  params.subgrid_krel) {
    MatFiller* matFiller = pm_amr.GetMatFiller();
    bool ret = matFiller != 0 && matFiller->Initialized();
    if (!ret) {
      BoxLib::Abort("RichardSolver::BuildMLPropEval: matFiller not ready");
    }

    const Array<BoxArray>& gridArray = layout.GridArray();
    int num_levs_mixed = matFiller->NumLevels();

    state_to_fill.resize(num_levs_mixed);
    derive_to_fill.resize(num_levs_mixed);

    for (int lev=0; lev<num_levs_mixed; ++lev) {
      const BoxArray& stf = (lev==0 ? gridArray[0] : state_to_fill[lev]);
      BoxArray mixed = BoxLib::intersect(matFiller->Mixed(lev),stf);
      if (mixed.size()>0) {
        if (lev<num_levs_mixed-1) {
          state_to_fill[lev+1] = BoxArray(mixed).refine(matFiller->RefRatio(lev));          
          BoxList bl(state_to_fill[lev+1]); bl.simplify(); state_to_fill[lev+1] = BoxArray(bl);
        }
        state_to_fill[lev] = ComplementIn(mixed,state_to_fill[lev],true);
      }
    }

    for (int lev=0; lev<num_levs_mixed; ++lev) {
      const BoxArray& stf = (lev==0 ? gridArray[0] : state_to_fill[lev]);
      BoxArray mixed = BoxLib::intersect(matFiller->Mixed(lev),stf);
      derive_to_fill[lev] = Join(state_to_fill[lev],mixed,true);
      if (lev>0) {
        derive_to_fill[lev] = Join(derive_to_fill[lev],
                                   BoxArray(matFiller->Mixed(lev-1)).refine(matFiller->RefRatio(lev-1)),true);
      }
    }

    for (int lev=0; lev<num_levs_mixed; ++lev) {
      int mg = pm_amr.maxGridSize(lev);
      if (state_to_fill[lev].size()>0) {
        state_to_fill[lev].maxSize(mg);
      }
      if (derive_to_fill[lev].size()>0) {
        derive_to_fill[lev].maxSize(mg);
      }
    }

    int num_fill = state_to_fill.size();
    phif.resize(num_fill,PArrayManage);
    pcPf.resize(num_fill,PArrayManage);
    kf.resize(num_fill,PArrayManage);
    krf.resize(num_fill,PArrayManage);
    pf.resize(num_fill,PArrayManage);
    lf.resize(num_fill,PArrayManage);

    for (int lev=1; lev<num_fill; ++lev) {
      const BoxArray& stff = state_to_fill[lev];
      if (stff.size()>0) {	
	int ncPhi = matFiller->nComp("porosity");
	phif.set(lev, new MultiFab(stff,ncPhi,0));

	int ncPcP = matFiller->nComp("capillary_pressure");
	pcPf.set(lev, new MultiFab(stff,ncPcP,0));

	int ncK = matFiller->nComp("permeability");
	kf.set(lev, new MultiFab(stff,ncK,0));

	int ncKr = matFiller->nComp("relative_permeability");
	krf.set(lev, new MultiFab(stff,ncKr,0));

	pf.set(lev, new MultiFab(stff,1,0));
	lf.set(lev, new MultiFab(stff,1,0));
      }
    }

    int num_derive = derive_to_fill.size();
    lc.resize(num_derive,PArrayManage);
    for (int lev=0; lev<num_derive; ++lev) {
      if (derive_to_fill[lev].size()>0) {
	lc.set(lev,new MultiFab(derive_to_fill[lev],BL_SPACEDIM,0));
      }
    }

  }
}

#undef __FUNCT__  
#define __FUNCT__ "RichardSolverDtr"
RichardSolver::~RichardSolver()
{
    PetscErrorCode ierr;

    delete Pnew;
    delete RhoSatNew;
    delete RhoSatOld;

    ierr = MatFDColoringDestroy(&matfdcoloring); CHKPETSC(ierr);
    ierr = ISColoringDestroy(&iscoloring);
    ierr = SNESDestroy(&snes); CHKPETSC(ierr);
    ierr = MatDestroy(&Jac); CHKPETSC(ierr);

    ierr = VecDestroy(&AlphaV); CHKPETSC(ierr);
    ierr = VecDestroy(&GV); CHKPETSC(ierr);
    ierr = VecDestroy(&SolnTypInvV); CHKPETSC(ierr);
    ierr = VecDestroy(&SolnTypV); CHKPETSC(ierr);
    ierr = VecDestroy(&SolnV); CHKPETSC(ierr);
    ierr = VecDestroy(&RhsV); CHKPETSC(ierr);

    for (int d=0; d<BL_SPACEDIM; ++d) {
        ctmp[d].clear();
    }

    DarcyVelocity.clear();
    RichardCoefs.clear();
    KappaEC.clear();

    delete Alpha;
    delete Rhs;
    delete Porosity;
    delete PCapParams;
    delete Lambda;
    delete KappaCCavg;
    delete KappaCCdir;

    delete mftfp;
}

static int dump_cnt = 0;

#undef __FUNCT__  
#define __FUNCT__ "Richard_SNESConverged"
PetscErrorCode Richard_SNESConverged(SNES snes, PetscInt it,PetscReal xnew_norm, 
                                     PetscReal dx_norm, PetscReal fnew_norm, 
                                     SNESConvergedReason *reason, void *ctx)
{
    CheckCtx *check_ctx = (CheckCtx *) ctx;
    RichardSolver* rs = check_ctx->rs;
    RichardNLSdata* nld = check_ctx->nld;
    const RSParams& rsp = rs->Parameters();

    PetscErrorCode ierr;

    PetscReal atol, rtol, stol;
    PetscInt maxit, maxf;

    static PetscReal dx_norm_0, fnew_norm_0;

    if (!rsp.ls_success) {
      *reason = SNES_DIVERGED_LINE_SEARCH;
    }
    else {
      ierr = SNESGetTolerances(snes,&atol,&rtol,&stol,&maxit,&maxf); CHKPETSC(ierr);
      ierr = SNESDefaultConverged(snes,it,xnew_norm,dx_norm,fnew_norm,reason,ctx); CHKPETSC(ierr);
    }

    if (*reason > 0) {
        dump_cnt = 0;
    }
    
    PetscFunctionReturn(ierr);
}

void
RichardSolver::ResetRemainingJacobianReuses()
{
    num_remaining_Jacobian_reuses = Parameters().max_num_Jacobian_reuses;
}

void
RichardSolver::UnsetRemainingJacobianReuses()
{
    num_remaining_Jacobian_reuses = 0;
}

bool 
RichardSolver::ReusePreviousJacobian()
{
    --num_remaining_Jacobian_reuses;
    if (ParallelDescriptor::IOProcessor() && num_remaining_Jacobian_reuses > 0) {
        std::cout << "Reusing J, " << num_remaining_Jacobian_reuses << " reuses left." << std::endl;
    }
    return (num_remaining_Jacobian_reuses > 0);
}

#undef __FUNCT__  
#define __FUNCT__ "Solve"
int
RichardSolver::Solve(Real cur_time, Real delta_t, int timestep, RichardNLSdata& nl_data)
{
  MFTower& RhsMFT = GetResidual();
  MFTower& SolnMFT = GetPressure();
  MFTower& PCapParamsMFT = GetPCapParams();

  Vec& RhsV = GetResidualV();
  Vec& SolnV = GetPressureV();
  Vec& SolnTypInvV = GetSolnTypInvV();

  // Copy from MFTowers in state to Vec structures
  PetscErrorCode ierr;
  ierr = layout.MFTowerToVec(RhsV,RhsMFT,0); CHKPETSC(ierr);
  ierr = layout.MFTowerToVec(SolnV,SolnMFT,0); CHKPETSC(ierr);
  ierr = layout.MFTowerToVec(SolnTypInvV,PCapParamsMFT,2); CHKPETSC(ierr); // sigma = 1/P_typ

  if (params.scale_soln_before_solve) {
      ierr = VecPointwiseMult(SolnV,SolnV,SolnTypInvV); // Mult(w,x,y): w=x.y  -- Scale IC
  }
  ierr = VecCopy(SolnTypInvV,SolnTypV); // Copy(x,y): y <- x
  ierr = VecReciprocal(SolnTypV); // Create vec to unscale as needed

  SetTime(cur_time);
  SetDt(delta_t);

  // Set permeability
  Array<PArray<MultiFab> > kappaEC(BL_SPACEDIM);
  if (params.upwind_krel) {
    for (int d=0; d<BL_SPACEDIM; ++d) {
      kappaEC[d].resize(nLevs,PArrayNoManage);
    }
    for (int lev=0; lev<nLevs; ++lev) {
      for (int d=0; d<BL_SPACEDIM; ++d) {
        kappaEC[d].set(lev,&(pm[lev].KappaEC()[d]));
      }
    }
    KappaEC.resize(BL_SPACEDIM, PArrayManage);
    for (int d=0; d<BL_SPACEDIM; ++d) {
      KappaEC.set(d, new MFTower(layout,kappaEC[d],nLevs));
    }
  }
  else {
    MatFiller* matFiller = pm_amr.GetMatFiller();
    bool ret = matFiller != 0 && matFiller->Initialized();
    KappaCCdir = new MFTower(layout,IndexType(IntVect::TheZeroVector()),BL_SPACEDIM,1,nLevs);
    for (int lev=0; lev<nLevs && ret; ++lev) {
      (*KappaCCdir)[lev].setVal(0);
      ret = matFiller->SetProperty(cur_time,lev,(*KappaCCdir)[lev],"permeability",0,1);
    }
    if (!ret) BoxLib::Abort("Failed to build permeability");

    int num_fill = state_to_fill.size();
    for (int lev=1; lev<num_fill; ++lev) {
      const BoxArray& stff = state_to_fill[lev];
      if (stff.size()>0) {	
	bool retPhi = matFiller->SetProperty(cur_time,lev,phif[lev],"porosity",0,0);
	bool retPc = matFiller->SetProperty(cur_time,lev,pcPf[lev],"capillary_pressure",0,0);
	bool retK = matFiller->SetProperty(cur_time,lev,kf[lev],"permeability",0,0);
	bool retKr = matFiller->SetProperty(cur_time,lev,krf[lev],"relative_permeability",0,0);
      }
    }
  }

  CheckCtx check_ctx;
  check_ctx.rs = this;
  check_ctx.nld = &nl_data;

  if (params.variable_switch_saturation_threshold>0) {
      ierr = SNESLineSearchSetPostCheck(snes,PostCheckAlt,(void *)(&check_ctx));CHKPETSC(ierr);
  }
  else {
      ierr = SNESLineSearchSetPostCheck(snes,PostCheck,(void *)(&check_ctx));CHKPETSC(ierr);  
  }
  ierr = SNESSetConvergenceTest(snes,Richard_SNESConverged,(void*)(&check_ctx),PETSC_NULL); CHKPETSC(ierr);

  UnsetRemainingJacobianReuses();

  // Evaluate the function
  PetscErrorCode (*func)(SNES,Vec,Vec,void*);
  void *fctx;
  ierr = SNESGetFunction(snes,PETSC_NULL,&func,&fctx);
  ierr = (*func)(snes,SolnV,RhsV,fctx); CHKPETSC(ierr);

  RichardSolver::SetTheRichardSolver(this);
  dump_cnt = 0;
  params.ls_success = true;
  ierr = SNESSolve(snes,PETSC_NULL,SolnV);// CHKPETSC(ierr);
  RichardSolver::SetTheRichardSolver(0);

  int iters;
  ierr = SNESGetIterationNumber(snes,&iters);CHKPETSC(ierr);
  nl_data.SetNLIterationsTaken(iters);

  SNESConvergedReason reason;
  ierr = SNESGetConvergedReason(snes,&reason); CHKPETSC(ierr);

  if (params.scale_soln_before_solve) {
      ierr = VecPointwiseMult(SolnV,SolnV,GetSolnTypV()); // Unscale current candidate solution
  }

  if (reason <= 0) {
      return reason;
  }

  // Copy solution from Vec back into state
  ierr = layout.VecToMFTower(SolnMFT,SolnV,0); CHKPETSC(ierr);

  return reason;
}

void
RichardSolver::ResetRhoSat()
{
  PArray<MultiFab> S_new(nLevs,PArrayNoManage);
  PArray<MultiFab> S_old(nLevs,PArrayNoManage);
  PArray<MultiFab> P_new(nLevs,PArrayNoManage);
  
  for (int lev=0; lev<nLevs; ++lev) {
    S_new.set(lev,&(pm[lev].get_new_data(State_Type)));
    S_old.set(lev,&(pm[lev].get_old_data(State_Type)));
    P_new.set(lev,&(pm[lev].get_new_data(Press_Type)));
  }

  delete RhoSatOld; RhoSatOld = new MFTower(layout,S_old,nLevs);
  delete RhoSatNew; RhoSatNew = new MFTower(layout,S_new,nLevs);
  delete Pnew; Pnew = new MFTower(layout,P_new,nLevs);
}

void
RichardSolver::SetTime(Real t) 
{
  mytime = t;
}

Real
RichardSolver::GetTime() const 
{
  return mytime;
}

void 
RichardSolver::SetDt(Real dt) 
{
  mydt = dt;
}

Real
RichardSolver::GetDt() const 
{
  return mydt;
}

#undef __FUNCT__  
#define __FUNCT__ "BuildOpSkel"
void
RichardSolver::BuildOpSkel(Mat& J)
{
  int num_rows = 1;
  int rows[1]; // At the moment, only set one row at a time
  Array<Real> vals;
  Array<int> cols;
  
  const Array<Geometry>& geomArray = layout.GeomArray();
  const Array<BoxArray>& gridArray = layout.GridArray();
  const Array<IntVect>& refRatio = layout.RefRatio();
  const PArray<Layout::MultiNodeFab>& nodes = layout.Nodes();
  const PArray<Layout::MultiIntFab>& nodeIds = layout.NodeIds();
  const Array<BoxArray>& bndryCells = layout.BndryCells();
  const Array<Array<IVSMap> >& growCellStencil = mftfp->GrowCellStencil();
  int nLevs = layout.NumLevels();
  
  PetscErrorCode ierr;
  int num_nbrs_reg = 2*BL_SPACEDIM+1;
  Layout::IntFab reg_neighbors;
  std::set<int> neighbors;
  typedef BaseFab<std::set<int> > ISetFab;
  typedef FabArray<ISetFab> MultiSetFab;
  PArray<MultiSetFab> crseContribs(nLevs,PArrayManage);
  
  int myproc = ParallelDescriptor::MyProc();
  int numprocs = ParallelDescriptor::NProcs();
  
  for (int lev=nLevs-1; lev>=0; --lev) 
    {
      const Array<IVSMap>& growCellStencilLev = growCellStencil[lev];
      const Layout::MultiNodeFab& nodeLev = nodes[lev];
      const Layout::MultiIntFab& nodeIdsLev = nodeIds[lev];

      Layout::MultiIntFab crseIds; // coarse cell ids at fine grid, distributed per fine patches
      crseContribs.set(lev,new MultiSetFab);
      if (lev>0) {
	BoxArray bacg = BoxArray(gridArray[lev]).coarsen(refRatio[lev-1]).grow(1);
	crseIds.define(bacg,1,0,Fab_allocate);
            
	const Layout::MultiIntFab& crseIds_orig = nodeIds[lev-1]; // crse cells through periodic boundary
	BoxArray gcba = BoxArray(crseIds_orig.boxArray()).grow(crseIds_orig.nGrow());
	Layout::MultiIntFab tmp(gcba,1,0);
	for (MFIter mfi(crseIds_orig); mfi.isValid(); ++mfi) {
	  tmp[mfi].copy(crseIds_orig[mfi]); // NOTE: Assumes grow cells already filled
	}
	crseIds.copy(tmp); // Parallel copy

	crseContribs[lev].define(bacg,1,0,Fab_allocate);
      }

      std::map<IntVect,std::set<int>,IntVect::Compare> stencil;
      if (lev<nLevs-1) {
	// Pack up the crseContribs for a parallel copy
	const BoxArray& ba = gridArray[lev];
	MultiSetFab& crseContribsFine = crseContribs[lev+1];
        const DistributionMapping& dm = nodeLev.DistributionMap();
	std::map<int,Array<int> > ccArrays;
	for (MFIter mfi(crseContribsFine); mfi.isValid(); ++mfi) {
	  const ISetFab& ccFab = crseContribsFine[mfi];
	  const Box& vbox = mfi.validbox();
	  std::vector< std::pair<int,Box> > isects = ba.intersections(vbox);
	  for (int i=0; i<isects.size(); ++i) {
            int dst_proc = dm[isects[i].first];

            // HACK  This was originally written for parallel, but when I tried it in serial, the entire 
            // crseContribs structure was ignored!!  For now, set this up as a communication, even if 
            // serial...probably an easy logic issue to clear up....famous last words...
	    if (1 || dst_proc != myproc) {
	      for (IntVect iv(vbox.smallEnd()), iEnd=vbox.bigEnd(); iv<=iEnd; vbox.next(iv))
		{
		  const std::set<int>& ids = ccFab(iv,0);
		  int thisSize = ids.size();
		  if (thisSize) {
		    Array<int>& ints = ccArrays[dst_proc];
		    int old_cc_size = ints.size();
		    int delta_cc = BL_SPACEDIM + 1 + ids.size();
		    int new_cc_size = old_cc_size + delta_cc;

		    ints.resize(new_cc_size);
		    for (int d=0; d<BL_SPACEDIM; ++d) {
		      ints[old_cc_size+d] = iv[d];
		    }
		    ints[old_cc_size+BL_SPACEDIM] = ids.size();
		    int cnt=0;
		    for (std::set<int>::const_iterator it=ids.begin(), End=ids.end(); it!=End; ++it, ++cnt) {
		      ints[old_cc_size+BL_SPACEDIM+1+cnt] = *it;
		    }
		  }
		}
	    }
	  }
	}

	int total_num_to_send = 0;
	Array<int> sends(numprocs,0);
	Array<int> soffsets(numprocs,0);
	for (int i=0; i<numprocs; ++i) {
	  sends[i] = ccArrays[i].size();
	  total_num_to_send += sends[i];
	  if (i>0) {
	    soffsets[i] = soffsets[i-1] + ccArrays[i-1].size();
	  }
	}
	Array<int> sbuf(total_num_to_send);
	for (int i=0; i<numprocs; ++i) {
	  for (int j=0; j<ccArrays[i].size(); ++j) {
	    sbuf[soffsets[i] + j] = ccArrays[i][j];
	  }
	}

	Array<int> recvs(numprocs);
	BL_MPI_REQUIRE( MPI_Alltoall(sends.dataPtr(),
				     1,
				     ParallelDescriptor::Mpi_typemap<int>::type(),
				     recvs.dataPtr(),
				     1,
				     ParallelDescriptor::Mpi_typemap<int>::type(),
				     ParallelDescriptor::Communicator()) );
            
	int total_num_to_recv = 0;
	Array<int> roffsets(numprocs,0);
	for (int i=0; i<numprocs; ++i) {
	  total_num_to_recv += recvs[i];
	  if (i>0) {
	    roffsets[i] = roffsets[i-1] + recvs[i-1];
	  }
	}
	Array<int> rbuf(total_num_to_recv);
	BL_MPI_REQUIRE( MPI_Alltoallv(total_num_to_send == 0 ? 0 : sbuf.dataPtr(),
				      sends.dataPtr(),
				      soffsets.dataPtr(),
				      ParallelDescriptor::Mpi_typemap<int>::type(),
				      total_num_to_recv == 0 ? 0 : rbuf.dataPtr(),
				      recvs.dataPtr(),
				      roffsets.dataPtr(),
				      ParallelDescriptor::Mpi_typemap<int>::type(),
				      ParallelDescriptor::Communicator()) );
            
	for (int i=0; i<numprocs; ++i) {
	  int jcnt = roffsets[i];
	  while (jcnt < roffsets[i] + recvs[i]) {
	    IntVect iv(&(rbuf[jcnt]));
	    int size = rbuf[jcnt+BL_SPACEDIM];
	    std::set<int>& iset = stencil[iv];
	    for (int k=0; k<size; ++k) {
	      iset.insert(rbuf[jcnt+BL_SPACEDIM+1+k]);
	    }
	    jcnt += BL_SPACEDIM+1+size;
	  }
	}
      }

      for (MFIter mfi(nodeLev); mfi.isValid(); ++mfi) {
	const Layout::NodeFab& nodeFab = nodeLev[mfi];
	const Layout::IntFab& nodeIdFab = nodeIdsLev[mfi];
	const Layout::IntFab* crseIdFab = (lev>0  ?  &(crseIds[mfi])  : 0);
	const Box& vbox = mfi.validbox();
	Box gbox = Box(vbox).grow(1);

	for (IntVect iv(vbox.smallEnd()), iEnd=vbox.bigEnd(); iv<=iEnd; vbox.next(iv))
	  {
	    const Node& nC = nodeFab(iv,0);
	    if (nC.type==Node::VALID) {
	      rows[0] = nodeIdFab(iv,0);
	      neighbors.clear();

	      std::map<IntVect,std::set<int>,IntVect::Compare>::const_iterator sit=stencil.find(iv);
	      if (sit!=stencil.end()) {
		const std::set<int>& iset = sit->second;
		neighbors.insert(iset.begin(),iset.end());
	      }
	      neighbors.insert(rows[0]);

	      for (int d=0; d<BL_SPACEDIM; ++d) {
		for (int pm = -1; pm<2; pm+=2) {
		  std::set<int> nd;
		  IntVect ivA = iv  +  pm * BoxLib::BASISV(d);
		  IVScit it=growCellStencilLev[d].find(ivA);
		  if (it!=growCellStencilLev[d].end()) {
		    const Stencil& s = it->second;
		    for (Stencil::const_iterator it=s.begin(), End=s.end(); it!=End; ++it) {
		      const Node& node = it->first;
		      const IntVect& ivs = node.iv;
		      int slev = node.level;
		      if (slev==lev) {
			BL_ASSERT(nodeIdFab.box().contains(ivs));
			int idx = nodeIdFab(ivs,0);
			if (ivs != iv && idx>=0) { // idx<0 is Dirichlet data, iv added above
			  nd.insert(idx);
			}
		      }
		      else if (slev==lev-1) {
			BL_ASSERT(crseIdFab);
			BL_ASSERT(crseIdFab->box().contains(ivs));
			nd.insert((*crseIdFab)(ivs,0));
		      }
		      else {
			std::cout << "stencil: " << s << std::endl;
			BoxLib::Abort("Bad stencil");
		      }
		    }

		    // contribute to coarse cell stencil, if appropriate
		    const Node& offcenter_node = nodeFab(ivA,0);
		    if (offcenter_node.type==Node::VALID  &&  offcenter_node.level==lev-1) {
		      crseContribs[lev][mfi](offcenter_node.iv,0).insert(rows[0]);
		      crseContribs[lev][mfi](offcenter_node.iv,0).insert(nd.begin(),nd.end());
		    }
		  }
		  else {
		    int idx = nodeIdFab(ivA,0);
		    if (idx>=0) { // idx<0 is a covered cell
		      neighbors.insert(idx);
		    }
		  }

		  // Merge this arm into full set
		  neighbors.insert(nd.begin(),nd.end());

		}
	      }

	      int num_cols = -1;
	      if (params.use_dense_Jacobian) 
		{
		  num_cols = layout.NumberOfGlobalNodeIds();
		  cols.resize(num_cols);
		  vals.resize(num_cols,0);
		  for (int i=0; i<num_cols; ++i) cols[i] = i;
		}
	      else
		{
		  num_cols = neighbors.size();
		  cols.resize(num_cols);
		  vals.resize(num_cols,0);
		  int cnt = 0;
		  for (std::set<int>::const_iterator it=neighbors.begin(), End=neighbors.end(); it!=End; ++it) {
		    cols[cnt++] = *it;
		  }
		}

	      ierr = MatSetValues(J,num_rows,rows,num_cols,cols.dataPtr(),vals.dataPtr(),INSERT_VALUES); CHKPETSC(ierr);
	    }
	  }
      }
    }

  ierr = MatAssemblyBegin(J,MAT_FINAL_ASSEMBLY); CHKPETSC(ierr);
  ierr = MatAssemblyEnd(J,MAT_FINAL_ASSEMBLY); CHKPETSC(ierr);
}

void
RichardSolver::CenterToEdgeUpwind(PArray<MFTower>&       mfte,
				  MFTower&               mftc,
				  const PArray<MFTower>& sgn,
				  int                    nComp,
                                  const BCRec&           bc) const
{
    for (int lev=0; lev<nLevs; ++lev) {
        MultiFab& clev = mftc[lev];
        BL_ASSERT(nComp<=clev.nComp());
        const Box& domain = GeomArray()[lev].Domain();
        for (MFIter mfi(clev); mfi.isValid(); ++mfi) {
            FArrayBox& cfab = clev[mfi];
            const Box& vccbox = mfi.validbox();
            for (int d=0; d<BL_SPACEDIM; ++d) {            
                FArrayBox& efab = mfte[d][lev][mfi];
                const FArrayBox& sgnfab = sgn[d][lev][mfi];
                BL_ASSERT(nComp<=efab.nComp());
                BL_ASSERT(nComp<=sgnfab.nComp());
                BL_ASSERT(Box(vccbox).surroundingNodes(d).contains(efab.box()));
                BL_ASSERT(Box(vccbox).surroundingNodes(d).contains(sgnfab.box()));
                
                int dir_bc_lo = bc.lo()[d]==EXT_DIR && vccbox.smallEnd()[d]==domain.smallEnd()[d];
                int dir_bc_hi = bc.hi()[d]==EXT_DIR && vccbox.bigEnd()[d]==domain.bigEnd()[d];

                int upwind_flag = (int)params.upwind_krel;
                FORT_RS_CTE_UPW(efab.dataPtr(), ARLIM(efab.loVect()), ARLIM(efab.hiVect()),
				cfab.dataPtr(), ARLIM(cfab.loVect()), ARLIM(cfab.hiVect()),
				sgnfab.dataPtr(),ARLIM(sgnfab.loVect()), ARLIM(sgnfab.hiVect()),
				vccbox.loVect(), vccbox.hiVect(), &d, &nComp, &dir_bc_lo, &dir_bc_hi,
				&upwind_flag);
            }
        }
    }
}

void 
RichardSolver::XmultYZ(MFTower&       X,
		       const MFTower& Y,
		       const MFTower& Z,
		       int            sCompY,
		       int            sCompZ,
		       int            dComp,
		       int            nComp,
		       int            nGrow)
{
  BL_ASSERT(layout.IsCompatible(X));
  BL_ASSERT(layout.IsCompatible(Y));
  BL_ASSERT(layout.IsCompatible(Z));
  BL_ASSERT(X.NComp()>=dComp+nComp);
  BL_ASSERT(Y.NComp()>=sCompY+nComp);
  BL_ASSERT(Z.NComp()>=sCompZ+nComp);
  BL_ASSERT(X.NGrow()>=nGrow);
  BL_ASSERT(Y.NGrow()>=nGrow);
  BL_ASSERT(Z.NGrow()>=nGrow);
  const Array<BoxArray>& gridArray = GridArray();
  const Array<Geometry>& geomArray = GeomArray();
  const Array<IntVect>& refRatio = RefRatio();

  FArrayBox tfabY, tfabZ;
  for (int lev=0; lev<nLevs; ++lev)
    {
      MultiFab& Xlev = X[lev];
      const MultiFab& Ylev = Y[lev];
      const MultiFab& Zlev = Z[lev];
      BoxArray fba;
      if (lev<nLevs-1) {
	fba = BoxArray(X[lev+1].boxArray()).coarsen(refRatio[lev]);
      }

      for (MFIter mfi(Y[lev]); mfi.isValid(); ++mfi)
        {
	  const Box& vbox = mfi.validbox();
	  Box gbox = Box(vbox).grow(nGrow);
	  FArrayBox& Xfab = Xlev[mfi];
	  const FArrayBox& Yfab = Ylev[mfi];
	  const FArrayBox& Zfab = Zlev[mfi];

	  tfabY.resize(gbox,nComp);
	  tfabZ.resize(gbox,nComp);
	  tfabY.copy(Yfab,sCompY,0,nComp);
	  tfabZ.copy(Zfab,sCompZ,0,nComp);

	  // Zero out parts of Y,Z covered by fine grid (to ensure valid data)
	  if (lev<nLevs-1) {
	    std::vector< std::pair<int,Box> > isects = fba.intersections(gbox);
	    for (int i = 0; i < isects.size(); i++)
	      {
		tfabY.setVal(0,isects[i].second,0,nComp);
		tfabZ.setVal(0,isects[i].second,0,nComp);
	      }
	  }

	  FORT_RS_XMULTYZ(Xfab.dataPtr(dComp),ARLIM(Xfab.loVect()), ARLIM(Xfab.hiVect()),
			  tfabY.dataPtr(),ARLIM(tfabY.loVect()), ARLIM(tfabY.hiVect()),
			  tfabZ.dataPtr(),ARLIM(tfabZ.loVect()), ARLIM(tfabZ.hiVect()),
			  gbox.loVect(), gbox.hiVect(), &nComp);
        }
    }
}



//
// Compute mfte[dir][comp] = Grad(mftc[comp]) + a[dir][comp]
//
void
RichardSolver::CCtoECgradAdd(PArray<MFTower>& mfte,
			     const MFTower&   mftc,
			     const FArrayBox& a,
			     int              sComp,
			     int              dComp,
			     int              nComp) const
{
  for (int d=0; d<BL_SPACEDIM; ++d) {            
    BL_ASSERT(layout.IsCompatible(mfte[d]));
  }
  BL_ASSERT(layout.IsCompatible(mftc));

  const Array<Geometry>& geomArray = layout.GeomArray();
  for (int lev=0; lev<nLevs; ++lev) {
    const MultiFab& mfc = mftc[lev];
    BL_ASSERT(mfc.nGrow()>=1);
    BL_ASSERT(sComp+nComp<=mfc.nComp());
    const Real* dx = geomArray[lev].CellSize();

    for (MFIter mfi(mfc); mfi.isValid(); ++mfi) {
      const FArrayBox& cfab = mfc[mfi];
      const Box& vcbox = mfi.validbox();
            
      for (int d=0; d<BL_SPACEDIM; ++d) {            
	FArrayBox& efab = mfte[d][lev][mfi];
	BL_ASSERT(dComp+nComp<=efab.nComp());
	BL_ASSERT(Box(vcbox).surroundingNodes(d).contains(efab.box()));
	efab.setVal(0);
	FORT_RS_GXPA(efab.dataPtr(dComp),ARLIM(efab.loVect()), ARLIM(efab.hiVect()),
		     cfab.dataPtr(sComp),ARLIM(cfab.loVect()), ARLIM(cfab.hiVect()),
		     vcbox.loVect(),vcbox.hiVect(),dx,a.dataPtr(d),&d,&nComp);
      }
    }
  }
}

void
RichardSolver::FillPatch(MFTower& mft,
			 int sComp,
			 int nComp,
			 bool do_piecewise_constant)
{
  mftfp->FillGrowCells(mft,sComp,nComp,do_piecewise_constant,nLevs);
}

void 
RichardSolver::SetInflowVelocity(PArray<MFTower>& velocity,
				 Real             t)
{
  for (int d=0; d<BL_SPACEDIM; ++d) {            
    BL_ASSERT(layout.IsCompatible(velocity[d]));
  }

  const Array<Geometry>& geomArray = layout.GeomArray();

  FArrayBox inflow;
  for (OrientationIter oitr; oitr; ++oitr) {
    Orientation face = oitr();
    int dir = face.coordDir();
    for (int lev=0; lev<nLevs; ++lev) {
      MultiFab& uld = velocity[dir][lev];
      if (pm[lev].get_inflow_velocity(face,inflow,t)) {
	int shift = ( face.isHigh() ? -1 : +1 );
	inflow.shiftHalf(dir,shift);
	for (MFIter mfi(uld); mfi.isValid(); ++mfi) {
	  FArrayBox& u = uld[mfi];
	  Box ovlp = inflow.box() & u.box();
	  if (ovlp.ok()) {
	    u.copy(inflow);
	  }
	}
      }
    }
  }
}

void 
RichardSolver::UpdateDarcyVelocity(MFTower& pressure,
				   Real     t)
{
  ComputeDarcyVelocity(GetDarcyVelocity(),pressure,GetRhoSatNp1(),
                       GetLambda(),GetKappaEC(),GetDensity(),GetGravity(),t);
}

void 
RichardSolver::ComputeDarcyVelocity(PArray<MFTower>&       darcy_vel,
				    MFTower&               pressure,
				    MFTower&               rhoSat,
				    MFTower&               lambda,
				    const PArray<MFTower>& kappa,
				    const Array<Real>&     rho,
				    const Array<Real>&     gravity,
				    Real                   t)
{
    // On Dirichlet boundaries, the grow cells of pressure will hold the value to apply at
    // the cell wall.  Note that since we use "calcInvPressure" to fill rho.sat, these
    // are then values on the wall as well.  As a result, the lambda values computed
    // with rho.sat are evaluated at the wall as well.  

    // We use the FillPatch operation to set pressure values in the grow cells using 
    // polynomial extrapolation, and will then use these p values only for the puposes
    // of evaluating the pressure gradient on cell faces via a simple centered difference.

    // Assumes lev=0 here corresponds to Amr.level=0, sets dirichlet values of rho.sat and
    // lambda on dirichlet pressure faces
    for (int lev=0; lev<nLevs; ++lev) {
        pm[lev].FillStateBndry(t,Press_Type,0,1); // Set new boundary data
        pm[lev].calcInvPressure(rhoSat[lev],pressure[lev]); // FIXME: Writes/reads only to comp=0, does 1 grow
    }

    int nComp = 1;
    Box abox(IntVect::TheZeroVector(),(nComp-1)*BoxLib::BASISV(0));
    FArrayBox a(abox,BL_SPACEDIM); // Make a funny box for a to simplify passing to Fortran
    for (int d=0; d<BL_SPACEDIM; ++d) {
        Real* ap = a.dataPtr(d);
        for (int n=0; n<nComp; ++n) {
            ap[n] = rho[n] * gravity[d];
        }
    }

    // Convert grow cells of pressure into extrapolated values so that from here on out,
    // the values are only used to compute gradients at faces.
    bool do_piecewise_constant = false;
    FillPatch(pressure,0,nComp,do_piecewise_constant);

    // Get  -(Grad(p) + rho.g)
    CCtoECgradAdd(darcy_vel,pressure,a);

    if (params.upwind_krel) {

      for (int lev=0; lev<nLevs; ++lev) {
        pm[lev].calcLambda(&(lambda[lev]),rhoSat[lev]); // FIXME: Writes/reads only to comp=0, does 1 grow
      }

      // Get edge-centered lambda (= krel/mu) based on the sign of -(Grad(p) + rho.g)
      const BCRec& pressure_bc = pm[0].get_desc_lst()[Press_Type].getBC(0);
      CenterToEdgeUpwind(GetRichardCoefs(),lambda,darcy_vel,nComp,pressure_bc);
      
      // Get Darcy velocity = - lambda * kappa * (Grad(p) + rho.g)
      for (int d=0; d<BL_SPACEDIM; ++d) {
        XmultYZ(darcy_vel[d],GetRichardCoefs()[d],kappa[d]);
      }    
    }
    else {

      for (int lev=0; lev<nLevs; ++lev) {
        pm[lev].calcLambda(&(lambda[lev]),rhoSat[lev]);
        MultiFab::Copy((*CoeffCC)[lev],lambda[lev],0,0,1,1);
        for (int d=1; d<BL_SPACEDIM; ++d) {
          MultiFab::Copy((*CoeffCC)[lev],(*CoeffCC)[lev],0,d,1,1);
        }
      }

      if (params.subgrid_krel) {
	const Array<IntVect>& refRatio = layout.RefRatio();
	const Array<BoxArray>& gridArray = layout.GridArray();
	const Array<Geometry>& geomArray = layout.GeomArray();
	MatFiller* matFiller = pm_amr.GetMatFiller();
	bool ret = matFiller != 0 && matFiller->Initialized();
	if (!ret) {
	  BoxLib::Abort("RichardSolver:: matFiller not ready");
	}      
	int num_levs_mixed = matFiller->NumLevels();
	int num_fill = state_to_fill.size();

	for (int lev=1; lev<num_fill; ++lev) {
	  if (state_to_fill[lev].size()>0) {

	    pm[lev].FillCoarsePatch(pf[lev],0,t,Press_Type,0,1);

	    FArrayBox rsf;
	    for (MFIter mfi(pf[lev]); mfi.isValid(); ++mfi) {
	      const FArrayBox& pfab = pf[lev][mfi];
	      rsf.resize(pfab.box(),1);
	      FArrayBox& lamf = lf[lev][mfi];
	      const FArrayBox& phifab = phif[lev][mfi];
	      const FArrayBox& kfab = kf[lev][mfi];
	      const FArrayBox& pcPfab = pcPf[lev][mfi];
	      const FArrayBox& krfab = krf[lev][mfi];
	      int ncPcP = pcPfab.nComp();
	      int ncKr = krfab.nComp();

	      FORT_MK_INV_CPL( pfab.dataPtr(),   ARLIM(pfab.loVect()),   ARLIM(pfab.hiVect()),
			       rsf.dataPtr(),    ARLIM(rsf.loVect()),    ARLIM(rsf.hiVect()),
			       phifab.dataPtr(), ARLIM(phifab.loVect()), ARLIM(phifab.hiVect()),
			       kfab.dataPtr(),   ARLIM(kfab.loVect()),   ARLIM(kfab.hiVect()),
			       pcPfab.dataPtr(), ARLIM(pcPfab.loVect()), ARLIM(pcPfab.hiVect()),
			       &ncPcP); 

	      FORT_MK_LAMBDA( lamf.dataPtr(),  ARLIM(lamf.loVect()),  ARLIM(lamf.hiVect()),
			      rsf.dataPtr(),   ARLIM(rsf.loVect()),   ARLIM(rsf.hiVect()),
			      krfab.dataPtr(), ARLIM(krfab.loVect()), ARLIM(krfab.hiVect()),
			      &ncKr);
	    }
	  }
	}

	// Average down, insert into CoeffCC
	int num_derive = derive_to_fill.size();
	for (int lev=num_derive-2; lev>=0; --lev) {
	  if (derive_to_fill[lev].size()>0) {
	    const IntVect& crat = matFiller->RefRatio(lev);
	    const BoxArray& cba = matFiller->Mixed(lev);
	    const BoxArray fba = BoxArray(cba).refine(crat);

	    MultiFab tlc(cba,BL_SPACEDIM,0);
	    MultiFab tlf(fba,BL_SPACEDIM,0);

	    tlf.setVal(-1);
	    tlf.copy(lf[lev+1],0,0,1);
	    for (int d=1; d<BL_SPACEDIM; ++d) {
	      tlf.copy(tlf,0,d,1);
	    }
	    if (lev<num_derive-2) {
	      tlf.copy(lc[lev+1],0,0,BL_SPACEDIM);
	    }

	    for (MFIter mfi(tlc); mfi.isValid(); ++mfi) {
	      const Box& crse_box = mfi.validbox();
	      const Box fine_box = Box(crse_box).refine(crat);
	      matFiller->CoarsenData(tlf[mfi],0,tlc[mfi],crse_box,0,BL_SPACEDIM,crat,
				     matFiller->coarsenRule("relative_permeability"));
	    }

	    lc[lev].copy(tlc,0,0,BL_SPACEDIM);
	    if (lev>0) {
	      for (int d=0; d<BL_SPACEDIM; ++d) {
		lc[lev].copy(lf[lev],0,d,1);
	      }
	    }

	    if (lev<nLevs) {
	      (*CoeffCC)[lev].copy(lc[lev],0,0,BL_SPACEDIM);
	    }
	  }
	}
      }

      // Make sure grow cells are consistent
      for (int lev=0; lev<nLevs; ++lev) {
	(*CoeffCC)[lev].FillBoundary(0,BL_SPACEDIM);
	geomArray[lev].FillPeriodicBoundary((*CoeffCC)[lev],0,BL_SPACEDIM);
      }

      // Get (lambda*kappa)
      for (int lev=0; lev<nLevs; ++lev) {
        MultiFab::Multiply((*CoeffCC)[lev],(*KappaCCdir)[lev],0,0,BL_SPACEDIM,1);
      }

      int do_harmonic = 1;
      int nComp = -1; // Note signal to take multiple components of cc to single comp of ec
      MFTower::CCtoECavg(GetRichardCoefs(),(*CoeffCC),1.0,0,0,nComp,do_harmonic);

      for (int lev=0; lev<nLevs; ++lev) {
        for (int d=0; d<BL_SPACEDIM; ++d) {
          MultiFab::Multiply(darcy_vel[d][lev],GetRichardCoefs()[d][lev],0,0,1,0);
        }
      }
    }

    // Overwrite face velocities at boundary with boundary conditions
    SetInflowVelocity(darcy_vel,t);

    // Average down velocities
    int sComp = 0;
    for (int d=0; d<BL_SPACEDIM; ++d) {
      MFTower::AverageDown(darcy_vel[d],sComp,nComp,nLevs);
    }    
}

Real TotalVolume()
{
    const RealBox& rb = Geometry::ProbDomain();
    Real vol = 1;
    for (int d=0; d<BL_SPACEDIM; ++d) {
        vol *= rb.length(d);
    }
    return vol;
}

void
RichardSolver::DivRhoU(MFTower& DivRhoU,
                       MFTower& pressure,
                       Real     t)
{
  // Get the Darcy flux
  ComputeDarcyVelocity(GetDarcyVelocity(),pressure,GetRhoSatNp1(),
                       GetLambda(),GetKappaEC(),GetDensity(),GetGravity(),t);

  // Get the divergence of the Darcy velocity flux = darcy vel . rho 
  //   leave velocity unscaled
  int sComp=0;
  int dComp=0;
  int nComp=1;
  MFTower::ECtoCCdiv(DivRhoU,GetDarcyVelocity(),GetDensity(),sComp,dComp,nComp,nLevs);
}

void
RichardSolver::DpDtResidual(MFTower& residual,
			    MFTower& pressure,
			    Real     t,
			    Real     dt)
{
  DivRhoU(residual,pressure,t);

  int sComp=0;
  int dComp=0;
  int nComp=1;

  const Array<BoxArray>& gridArray = layout.GridArray();
  const Array<IntVect>& refRatio = layout.RefRatio();

  for (int lev=0; lev<nLevs; ++lev)
    {
      MultiFab& Rlev = residual[lev];
      for (MFIter mfi(Rlev); mfi.isValid(); ++mfi) {
	const Box& vbox = mfi.validbox();
	FArrayBox& Res = Rlev[mfi];
	const FArrayBox& rs_n = GetRhoSatN()[lev][mfi];
	const FArrayBox& rs_np1 = GetRhoSatNp1()[lev][mfi];
	const FArrayBox& phi_n = GetPorosity()[lev][mfi];
	const FArrayBox& phi_np1 = GetPorosity()[lev][mfi];
	FORT_RS_PDOTRES(Res.dataPtr(),    ARLIM(Res.loVect()),     ARLIM(Res.hiVect()),
			rs_n.dataPtr(),   ARLIM(rs_n.loVect()),    ARLIM(rs_n.hiVect()),
			rs_np1.dataPtr(), ARLIM(rs_np1.loVect()),  ARLIM(rs_np1.hiVect()),
			phi_n.dataPtr(),  ARLIM(phi_n.loVect()),   ARLIM(phi_n.hiVect()),
			phi_np1.dataPtr(),ARLIM(phi_np1.loVect()), ARLIM(phi_np1.hiVect()),
		        &dt, vbox.loVect(), vbox.hiVect(), &nComp);
        }
    }
}

#undef __FUNCT__  
#define __FUNCT__ "CreatJac"
void RichardSolver::CreateJac(Mat& J, 
			      MFTower& pressure,
			      Real dt)
{
  const Array<BoxArray>& gridArray = layout.GridArray();
  const Array<IntVect>& refRatio   = layout.RefRatio();
  BaseFab<int> nodeNums;
  PetscErrorCode ierr;
  const BCRec& theBC = pm[0].get_desc_lst()[Press_Type].getBC(0);
  PArray<MultiFab> kr_params(nLevs,PArrayNoManage);
  
  for (int lev=0; lev<nLevs; ++lev) {
    kr_params.set(lev, pm[lev].KrParams());
  }
  MFTower& PCapParamsMFT = GetPCapParams();
  MFTower KrParamsMFT(layout,kr_params,nLevs);

  const Array<int>& rinflow_bc_lo = pm[0].rinflowBCLo();
  const Array<int>& rinflow_bc_hi = pm[0].rinflowBCHi();

  int do_upwind = (int)params.upwind_krel;
  for (int lev=0; lev<nLevs; ++lev) {
    const Box& domain = GeomArray()[lev].Domain();
    const Real* dx = GeomArray()[lev].CellSize();
    MultiFab& Plev = pressure[lev];

    PArray<MultiFab> jacflux;
    jacflux.resize(BL_SPACEDIM,PArrayManage);
    for (int d=0; d<BL_SPACEDIM; ++d) {
      BoxArray ba = BoxArray(pressure[lev].boxArray()).surroundingNodes(d);
      jacflux.set(d,new MultiFab(ba,3,0));
    }

    // may not necessary since this should be same as the residual
    pm[lev].calcInvPressure(GetRhoSatNp1()[lev],pressure[lev]); 
    pm[lev].calcLambda(&(GetLambda()[lev]),GetRhoSatNp1()[lev]); 

    for (MFIter mfi(Plev); mfi.isValid(); ++mfi) {
      const Box& vbox = mfi.validbox();
      const int idx   = mfi.index();
      Array<int> bc   = pm[lev].getBCArray(Press_Type,idx,0,1);      

      Box gbox = Box(vbox).grow(1);
      nodeNums.resize(gbox,1);
      layout.SetNodeIds(nodeNums,lev,idx);

      // reusing RichardCoefs to store Jacobian flux term
      FArrayBox& jfabx = jacflux[0][mfi];
      FArrayBox& jfaby = jacflux[1][mfi];
      FArrayBox& vfabx = GetDarcyVelocity()[0][lev][mfi];
      FArrayBox& vfaby = GetDarcyVelocity()[1][lev][mfi];
      FArrayBox& kfabx = GetKappaEC()[0][lev][mfi];
      FArrayBox& kfaby = GetKappaEC()[1][lev][mfi];
      
#if (BL_SPACEDIM==3)
      FArrayBox& jfabz = jacflux[2][mfi];
      FArrayBox& vfabz = GetDarcyVelocity()[2][lev][mfi];
      FArrayBox& kfabz = GetKappaEC()[2][lev][mfi];
#endif
      FArrayBox& ldfab = GetLambda()[lev][mfi];
      FArrayBox& prfab = pressure[lev][mfi];
      FArrayBox& pofab = GetPorosity()[lev][mfi];
      FArrayBox& kcfab = GetKappaCCavg() [lev][mfi];
      FArrayBox& cpfab = PCapParamsMFT[lev][mfi];
      const int n_cp_coef = cpfab.nComp();
      FArrayBox& krfab = KrParamsMFT[lev][mfi];
      const int n_kr_coef = krfab.nComp();
      Real deps = 1.e-8;

      FORT_RICHARD_NJAC2(jfabx.dataPtr(), ARLIM(jfabx.loVect()),ARLIM(jfabx.hiVect()),
			 jfaby.dataPtr(), ARLIM(jfaby.loVect()),ARLIM(jfaby.hiVect()),

#if(BL_SPACEDIM==3)
			 jfabz.dataPtr(), ARLIM(jfabz.loVect()),ARLIM(jfabz.hiVect()),
#endif	
			 vfabx.dataPtr(), ARLIM(vfabx.loVect()),ARLIM(vfabx.hiVect()),
			 vfaby.dataPtr(), ARLIM(vfaby.loVect()),ARLIM(vfaby.hiVect()),
#if(BL_SPACEDIM==3)
			 vfabz.dataPtr(), ARLIM(vfabz.loVect()),ARLIM(vfabz.hiVect()),
#endif
			 kfabx.dataPtr(), ARLIM(kfabx.loVect()),ARLIM(kfabx.hiVect()),
			 kfaby.dataPtr(), ARLIM(kfaby.loVect()),ARLIM(kfaby.hiVect()),
#if(BL_SPACEDIM==3)
			 kfabz.dataPtr(), ARLIM(kfabz.loVect()),ARLIM(kfabz.hiVect()),  
#endif
			 ldfab.dataPtr(), ARLIM(ldfab.loVect()),ARLIM(ldfab.hiVect()),
			 
			 prfab.dataPtr(), ARLIM(prfab.loVect()),ARLIM(prfab.hiVect()),
			 pofab.dataPtr(), ARLIM(pofab.loVect()),ARLIM(pofab.hiVect()),
			 kcfab.dataPtr(), ARLIM(kcfab.loVect()),ARLIM(kcfab.hiVect()),
			 krfab.dataPtr(), ARLIM(krfab.loVect()),ARLIM(krfab.hiVect()), &n_kr_coef,
			 cpfab.dataPtr(), ARLIM(cpfab.loVect()),ARLIM(cpfab.hiVect()), &n_cp_coef,
			 vbox.loVect(), vbox.hiVect(), domain.loVect(), domain.hiVect(), 
			 dx, bc.dataPtr(), 
			 rinflow_bc_lo.dataPtr(),rinflow_bc_hi.dataPtr(), 
			 &deps, &do_upwind);


      FArrayBox dalpha(gbox,1);
      FArrayBox& nfab = GetRhoSatNp1()[lev][mfi];
      
      FORT_RICHARD_ALPHA(dalpha.dataPtr(), ARLIM(dalpha.loVect()), ARLIM(dalpha.hiVect()),
			 nfab.dataPtr(), ARLIM(nfab.loVect()),ARLIM(nfab.hiVect()),
			 pofab.dataPtr(), ARLIM(pofab.loVect()),ARLIM(pofab.hiVect()),
			 kcfab.dataPtr(), ARLIM(kcfab.loVect()), ARLIM(kcfab.hiVect()),
			 cpfab.dataPtr(), ARLIM(cpfab.loVect()), ARLIM(cpfab.hiVect()), &n_cp_coef,
			 vbox.loVect(), vbox.hiVect());
      
      Array<int> cols(1+2*BL_SPACEDIM);
      Array<int> rows(1);
      Array<Real> vals(cols.size(),0);

      const Array<double>& rho = GetDensity();
      int nc = 0;

      for (IntVect iv(vbox.smallEnd()), iEnd=vbox.bigEnd(); iv<=iEnd; vbox.next(iv))
      {
	  cols[0] = nodeNums(iv,0);
          if (cols[0]>=0) {
              rows[0] = cols[0];
              vals[0] = dalpha(iv,0);
              Real rdt = (dt>0  ?  rho[nc]*dt : 1); // The "b" factor
              int cnt = 1;
              for (int d=0; d<BL_SPACEDIM; ++d) {
                  vals[0] -= rdt * jacflux[d][mfi](iv,2);
                  IntVect ivp = iv + BoxLib::BASISV(d);
                  int np = nodeNums(ivp,0);
                  if (np>=0) {
                      cols[cnt]  = np; 
                      vals[cnt]  = -rdt * jacflux[d][mfi](iv,0);
                      cnt++;
                  }
                  else {
                      if (theBC.hi()[d]==FOEXTRAP) {
                          vals[0] -= rdt * jacflux[d][mfi](iv,0);
                      }
                  }
                  
                  IntVect ivn = iv - BoxLib::BASISV(d);
                  int nn = nodeNums(ivn,0);
                  if (nn>=0) {
                      cols[cnt]  = nn; 
                      vals[cnt]  = -rdt * jacflux[d][mfi](iv,1);
                      cnt++;
                  }
                  else {
                      if (theBC.lo()[d]==FOEXTRAP) {
                          vals[0] -= rdt * jacflux[d][mfi](iv,1);
                      }
                  }
              }
              ierr = MatSetValues(J,rows.size(),rows.dataPtr(),cnt,cols.dataPtr(),vals.dataPtr(),INSERT_VALUES); CHKPETSC(ierr);
	  }
      }
    }
  }
  ierr = MatAssemblyBegin(J,MAT_FINAL_ASSEMBLY); CHKPETSC(ierr);
  ierr = MatAssemblyEnd(J,MAT_FINAL_ASSEMBLY); CHKPETSC(ierr);
}

#undef __FUNCT__  
#define __FUNCT__ "RichardRes_DpDt"
PetscErrorCode 
RichardRes_DpDt(SNES snes,Vec x,Vec f,void *dummy)
{
    PetscErrorCode ierr; 
    RichardSolver* rs = static_cast<RichardSolver*>(dummy);
    if (!rs) {
        BoxLib::Abort("Bad cast in RichardRes_DpDt");
    }

    if (rs->Parameters().scale_soln_before_solve) {
        ierr = VecPointwiseMult(x,x,rs->GetSolnTypV()); CHKPETSC(ierr); // Unscale solution
    }

    MFTower& xMFT = rs->GetPressure();
    MFTower& fMFT = rs->GetResidual();

    Layout& layout = rs->GetLayout();
    ierr = layout.VecToMFTower(xMFT,x,0); CHKPETSC(ierr);

    Real t = rs->GetTime();
    Real dt = rs->GetDt();
    rs->DpDtResidual(fMFT,xMFT,t,dt);

#if 0
    // Scale residual by cell volume/sqrt(total volume)
    Real sqrt_total_volume_inv = std::sqrt(1/TotalVolume());
    int nComp = 1;
    int nLevs = rs->GetNumLevels();
    for (int lev=0; lev<nLevs; ++lev)
    {
      MultiFab::Multiply(fMFT[lev],layout.Volume(lev),0,0,nComp,0);
      fMFT[lev].mult(sqrt_total_volume_inv,0,1);
    }
#endif

    ierr = layout.MFTowerToVec(f,fMFT,0); CHKPETSC(ierr);

    if (rs->Parameters().scale_soln_before_solve) {
        ierr = VecPointwiseMult(x,x,rs->GetSolnTypInvV()); CHKPETSC(ierr); // Reset solution scaling
    }
    PetscFunctionReturn(0);
}

#undef __FUNCT__  
#define __FUNCT__ "RichardR2"
PetscErrorCode 
RichardR2(SNES snes,Vec x,Vec f,void *dummy)
{
    PetscErrorCode ierr; 
    RichardSolver* rs = static_cast<RichardSolver*>(dummy);
    if (!rs) {
        BoxLib::Abort("Bad cast in RichardR2");
    }

    if (rs->Parameters().scale_soln_before_solve) {
        ierr = VecPointwiseMult(x,x,rs->GetSolnTypV()); CHKPETSC(ierr); // Unscale solution
    }

    MFTower& xMFT = rs->GetPressure();
    MFTower& fMFT = rs->GetResidual();

    Layout& layout = rs->GetLayout();
    ierr = layout.VecToMFTower(xMFT,x,0); CHKPETSC(ierr);

    Real t = rs->GetTime();
    rs->DivRhoU(fMFT,xMFT,t);

    ierr = layout.MFTowerToVec(f,fMFT,0); CHKPETSC(ierr);

    if (rs->Parameters().scale_soln_before_solve) {
        ierr = VecPointwiseMult(x,x,rs->GetSolnTypInvV()); CHKPETSC(ierr); // Reset solution scaling
    }
    PetscFunctionReturn(0);
}

#if defined(PETSC_3_2)

#undef __FUNCT__  
#define __FUNCT__ "RichardJacFromPM"
PetscErrorCode 
RichardJacFromPM(SNES snes, Vec x, Mat* jac, Mat* jacpre, MatStructure* flag, void *dummy)
{
  PetscErrorCode ierr;
  RichardSolver* rs = static_cast<RichardSolver*>(dummy);
  if (!rs) {
    BoxLib::Abort("Bad cast in RichardJacFromPM");
  }
  MFTower& xMFT = rs->GetPressure();
  
  Layout& layout = rs->GetLayout();
  ierr = layout.VecToMFTower(xMFT,x,0); CHKPETSC(ierr);
  Real dt = rs->GetDt();
  rs->CreateJac(*jacpre,xMFT,dt);
  if (*jac != *jacpre) {
    ierr = MatAssemblyBegin(*jac,MAT_FINAL_ASSEMBLY);CHKPETSC(ierr);
    ierr = MatAssemblyEnd(*jac,MAT_FINAL_ASSEMBLY);CHKPETSC(ierr);
  }
  PetscFunctionReturn(0);
} 

#undef __FUNCT__  
#define __FUNCT__ "RecordSolve"
void RecordSolve(Vec& p,Vec& dp,Vec& dp_orig,Vec& pnew,Vec& F,Vec& G,CheckCtx* check_ctx)
{
  RichardSolver* rs = check_ctx->rs;
  const std::string& record_file = rs->GetRecordFile();
  BL_ASSERT(!record_file.empty());
  Layout& layout = check_ctx->rs->GetLayout();
  int nLevs = rs->GetNumLevels();

  int num_out = 9;
  Array<MFTower*> dMFT(num_out);
  Array<std::string> names(num_out);

  PetscErrorCode ierr;
  for (int i=0; i<num_out; ++i) {
    dMFT[i] = new MFTower(layout,IndexType(IntVect::TheZeroVector()),1,1,nLevs);
  }

  MFTower& ResMFT     = *(dMFT[0]);
  MFTower& DpMFT      = *(dMFT[1]);
  MFTower& Dp_origMFT = *(dMFT[2]);
  MFTower& PoldMFT    = *(dMFT[3]);
  MFTower& PnewMFT    = *(dMFT[4]);
  MFTower& SnewMFT    = *(dMFT[5]);
  MFTower& SoldMFT    = *(dMFT[6]);
  MFTower& DsMFT      = *(dMFT[7]);
  MFTower& fMFT       = *(dMFT[8]);

  ierr = layout.VecToMFTower(    ResMFT,      G,0); CHKPETSC(ierr);
  ierr = layout.VecToMFTower(     DpMFT,     dp,0); CHKPETSC(ierr);
  ierr = layout.VecToMFTower(Dp_origMFT,dp_orig,0); CHKPETSC(ierr);
  ierr = layout.VecToMFTower(   PoldMFT,      p,0); CHKPETSC(ierr);
  ierr = layout.VecToMFTower(   PnewMFT,   pnew,0); CHKPETSC(ierr);

  Real cur_time = rs->GetTime();
  Real rho = rs->GetDensity()[0];
  PArray<PorousMedia>& pm = rs->PMArray();
  for (int lev=0; lev<nLevs; ++lev) {
    pm[lev].FillStateBndry(cur_time,Press_Type,0,1);
    pm[lev].calcInvPressure(SnewMFT[lev],PnewMFT[lev]);
    SnewMFT[lev].mult(1/rho,0,1);

    pm[lev].calcInvPressure(SoldMFT[lev],PoldMFT[lev]);
    SoldMFT[lev].mult(1/rho,0,1);

    MultiFab::Copy(DsMFT[lev],SnewMFT[lev],0,0,1,0);
    MultiFab::Subtract(DsMFT[lev],SoldMFT[lev],0,0,1,0);

    for (MFIter mfi(fMFT[lev]); mfi.isValid(); ++mfi) {
      const Box& box = mfi.validbox();
      for (IntVect iv=box.smallEnd(), End=box.bigEnd(); iv<=End; box.next(iv)) {
	const Real& num = DpMFT[lev][mfi](iv,0);
	const Real& den = Dp_origMFT[lev][mfi](iv,0);
	fMFT[lev][mfi](iv,0) = den==0 ? 1 : std::abs(num/den);
      }
    }
  }

  for (int i=0; i<num_out; ++i) {
    dMFT[i]->SetValCovered(0);
  }

  names[0] = "Res_undamped";
  names[1] = "Dp_damped";
  names[2] = "Dp_undamped";
  names[3] = "Pold";
  names[4] = "Pnew_damped";
  names[5] = "Snew_damped";
  names[6] = "Sold";
  names[7] = "dS";
  names[8] = "DampingFactor";

  int timestep = rs->GetCurrentTimestep();
  std::string step_file = BoxLib::Concatenate(record_file + "/Step_",timestep,3);
  step_file = BoxLib::Concatenate(step_file + "/iteration_",dump_cnt,3);

  if (ParallelDescriptor::IOProcessor()) {
    std::cout << "****************** Writing file: " << step_file << std::endl;
  }
  Real time = 0;
  MFTower::WriteSet(step_file,dMFT,names,time);
  dump_cnt++;
}

/*
   PostCheck - User-defined routine that checks the validity of
   candidate steps of a line search method.  Set by SNESLineSearchSetPostCheck().
   In:
   snes 	- nonlinear context
   checkctx 	- optional user-defined context for use by step checking routine
   x     	- previous iterate
   y 	        - new search direction and length
   w 	        - current candidate iterate
   
   Out:
   y            - search direction (possibly changed)
   w            - current iterate (possibly modified)
   changed_y 	- indicates search direction was changed by this routine
   changed_w 	- indicates current iterate was changed by this routine 

 */

#undef __FUNCT__  
#define __FUNCT__ "PostCheck"
PetscErrorCode 
PostCheck(SNES snes,Vec x,Vec y,Vec w,void *ctx,PetscBool  *changed_y,PetscBool  *changed_w)
{
    std::string tag = "       Newton step: ";
    std::string tag_ls = "  line-search:  ";
    CheckCtx* check_ctx = (CheckCtx*)ctx;
    RichardSolver* rs = check_ctx->rs;
    RichardNLSdata* nld = check_ctx->nld;
    RSParams& rsp = rs->Parameters();

    if (rs==0) {
        BoxLib::Abort("Context cast failed in PostCheck");
    }

    rsp.ls_success = true;
    rsp.ls_reason = "In Progress";

    PetscErrorCode ierr;
    PetscReal fnorm, xnorm, ynorm, gnorm;

    PetscErrorCode (*func)(SNES,Vec,Vec,void*);
    void *fctx;

    ierr = SNESGetFunction(snes,PETSC_NULL,&func,&fctx); CHKPETSC(ierr);

    Vec& F = rs->GetResidualV();
    Vec& G = rs->GetTrialResV();
    
    ierr = (*func)(snes,x,F,fctx); CHKPETSC(ierr);
    ierr = VecNorm(F,NORM_2,&fnorm); CHKPETSC(ierr);

    ierr = (*func)(snes,w,G,fctx); CHKPETSC(ierr);
    ierr = VecNorm(G,NORM_2,&gnorm);  CHKPETSC(ierr);

    Vec y_orig;
    if (!(rs->GetRecordFile().empty())) {
      ierr = VecDuplicate(y,&y_orig); CHKPETSC(ierr);
      ierr = VecCopy(y,y_orig); CHKPETSC(ierr);
    }

    bool norm_acceptable = gnorm < fnorm * rsp.ls_acceptance_factor;
    int ls_iterations = 0;
    Real ls_factor = 1;
    bool finished = norm_acceptable 
        || ls_iterations > rsp.max_ls_iterations
        || ls_factor <= rsp.min_ls_factor;

    Real gnorm_0 = gnorm;
    while (!finished) 
    {
        ls_factor *= rsp.ls_reduction_factor;
        if (ls_factor < rsp.min_ls_factor) {
            ls_factor = rsp.min_ls_factor;
        }

        PetscReal mone = -1;
        ierr=VecWAXPY(w,mone*ls_factor,y,x); CHKPETSC(ierr); /* w = -y + x */
        *changed_w = PETSC_TRUE;
        
        ierr = (*func)(snes,w,G,fctx); CHKPETSC(ierr);
        ierr=VecNorm(G,NORM_2,&gnorm);CHKPETSC(ierr); CHKPETSC(ierr);
        norm_acceptable = gnorm < fnorm * rsp.ls_acceptance_factor;
        
        if (ls_factor < 1 
            && rsp.monitor_line_search 
            && ParallelDescriptor::IOProcessor())
	{
            std::cout << tag << tag_ls
                      << "iter=" << ls_iterations
                      << ", step length=" << ls_factor
                      << ", Newton norm=" << gnorm_0
                      << ", damped norm=" << gnorm << '\n';
	}
        
        finished = norm_acceptable 
            || ls_iterations > rsp.max_ls_iterations
            || ls_factor <= rsp.min_ls_factor;      
        ls_iterations++;
    }
    
    if (ls_iterations > rsp.max_ls_iterations) 
    {
        std::string reason = "Solution rejected.  Linear system solved, but ls_iterations too large";
        if (ParallelDescriptor::IOProcessor() && rsp.monitor_line_search) {
            std::cout << tag << tag_ls << reason << std::endl;
        }
        rsp.ls_success = false;
        rsp.ls_reason = reason;
    }
    else if (ls_factor <= rsp.min_ls_factor) {
        std::string reason = "Solution rejected.  Linear system solved, but ls_factor too small";
        if (ParallelDescriptor::IOProcessor() && rsp.monitor_line_search) {
            std::cout << tag << tag_ls << reason << std::endl;
        }
        rsp.ls_success = false;
        rsp.ls_reason = reason;
    }
    else {
        if (ls_factor == 1) {
            std::string reason = "Full linear step accepted";
            if (ParallelDescriptor::IOProcessor() && rsp.monitor_line_search>1) {
                std::cout << tag << tag_ls << reason << std::endl;
            }
            rsp.ls_reason = reason;
        }
        else {
            // Set update to the one actually used
            ierr=VecScale(y,ls_factor); CHKPETSC(ierr);
            *changed_y = PETSC_TRUE;
            rsp.ls_reason = "Damped step successful";
        }
        rsp.ls_success = true;

        int iters = nld->NLIterationsTaken() + 1;
    }

    if (!(rs->GetRecordFile().empty())) {
      RecordSolve(x,y,y_orig,w,F,G,check_ctx);
      ierr = VecDestroy(&y_orig); CHKPETSC(ierr);
    }
    
    PetscFunctionReturn(0);
}

#undef __FUNCT__  
#define __FUNCT__ "AltUpdate"
PetscErrorCode
AltUpdate(SNES snes,Vec pk,Vec dp,Vec pkp1,void *ctx,Real ls_factor,PetscBool *changed_dp,PetscBool *changed_pkp1)
{
    PetscErrorCode ierr;
    CheckCtx* check_ctx = (CheckCtx*)ctx;
    RichardSolver* rs = check_ctx->rs;
    RichardNLSdata* nld = check_ctx->nld;
    if (rs==0) {
        BoxLib::Abort("Context cast failed in AltUpdate");
    }
    RSParams& rsp = rs->Parameters();

    rsp.ls_success = true;
    rsp.ls_reason = "In Progress";

    if (rs->Parameters().scale_soln_before_solve) {
        Vec& Ptyp = rs->GetSolnTypV();
        ierr = VecPointwiseMult(dp,dp,Ptyp); CHKPETSC(ierr);
        ierr = VecPointwiseMult(pk,pk,Ptyp); CHKPETSC(ierr);
    }

    MFTower& P_MFT = rs->GetPressure();
    MFTower& RS_MFT = rs->GetRhoSatNp1();
    MFTower& DP_MFT = rs->GetAlpha(); //Handy data container
    const MFTower& K_MFT = rs->GetKappaCCavg();

    Layout& layout = rs->GetLayout();
    ierr = layout.VecToMFTower(P_MFT,pk,0); CHKPETSC(ierr);
    ierr = layout.VecToMFTower(DP_MFT,dp,0); CHKPETSC(ierr);

    int nLevs = layout.NumLevels();
    for (int lev=0; lev<nLevs; ++lev) {

        // Fill (rho.sat)^{n+1,k} from p^{n+1,k}
        rs->GetPMlevel(lev).calcInvPressure(RS_MFT[lev],P_MFT[lev]);  
  
        // Get full set of Pcap parameters directly
        // (not just the sigma part stored in my copy of PCapParams)
        const MultiFab* PCapParams = rs->GetPMlevel(lev).PCapParams();

        // Compute the "Alternating Update" according to Krabbenhoft, AWR30 p.483
        for (MFIter mfi(P_MFT[lev]); mfi.isValid(); ++mfi) {
            const Box& vbox = mfi.validbox();
            const FArrayBox& kcf = K_MFT[lev][mfi];
            const FArrayBox& cpf = (*PCapParams)[mfi];
            int n_cp_coefs = cpf.nComp();
            
            FArrayBox& rsf = RS_MFT[lev][mfi];
            FArrayBox& pf  = P_MFT[lev][mfi];
            FArrayBox& dpf = DP_MFT[lev][mfi];

            FORT_RS_ALTUP(rsf.dataPtr(),ARLIM(rsf.loVect()), ARLIM(rsf.hiVect()),
                          pf.dataPtr(), ARLIM(pf.loVect()),  ARLIM(pf.hiVect()),
                          dpf.dataPtr(),ARLIM(dpf.loVect()), ARLIM(dpf.hiVect()),
                          kcf.dataPtr(),ARLIM(kcf.loVect()), ARLIM(kcf.hiVect()),
                          cpf.dataPtr(),ARLIM(cpf.loVect()), ARLIM(cpf.hiVect()),
                          &n_cp_coefs, &ls_factor, vbox.loVect(), vbox.hiVect(),
                          &(rs->Parameters()).variable_switch_saturation_threshold);
        }
    }
    
    // Put modified dp into Vec
    ierr = layout.MFTowerToVec(dp,DP_MFT,0); CHKPETSC(ierr);

    // Compute new p = p - dp, then scale all p, pnew and dt
    ierr = VecWAXPY(pkp1,-1.0,dp,pk);CHKPETSC(ierr);

    if (rs->Parameters().scale_soln_before_solve) {
        Vec& PtypInv = rs->GetSolnTypInvV();
        ierr = VecPointwiseMult(dp,dp,PtypInv); CHKPETSC(ierr);
        ierr = VecPointwiseMult(pk,pk,PtypInv); CHKPETSC(ierr);
        ierr = VecPointwiseMult(pkp1,pkp1,PtypInv); CHKPETSC(ierr);
    }
    *changed_dp = PETSC_FALSE; // We changed dp and pnew, but we took care of the update already
    *changed_pkp1 = PETSC_TRUE;

    rsp.ls_success = true;
    rsp.ls_reason = "Damped step successful";

    PetscFunctionReturn(0);
}

#if defined(PETSC_3_2)
#include <private/snesimpl.h>
#else
#include <petsc-private/snesimpl.h> 
#endif

#undef __FUNCT__  
#define __FUNCT__ "PostCheckAlt"
PetscErrorCode 
PostCheckAlt(SNES snes,Vec p,Vec dp,Vec pnew,void *ctx,PetscBool  *changed_dp,PetscBool  *changed_pnew)
{
    std::string tag = "       Newton step: ";
    std::string tag_ls = "  line-search:  ";
    CheckCtx* check_ctx = (CheckCtx*)ctx;
    RichardSolver* rs = check_ctx->rs;
    RichardNLSdata* nld = check_ctx->nld;
    if (rs==0) {
        BoxLib::Abort("Context cast failed in PostCheckAlt");
    }
    RSParams& rsp = rs->Parameters();

    rsp.ls_success = true;
    rsp.ls_reason = "In Progress";

    PetscErrorCode ierr;
    PetscReal fnorm, xnorm, ynorm, gnorm;
    PetscErrorCode (*func)(SNES,Vec,Vec,void*);
    void *fctx;

    Real ls_factor = 1;
    ierr = AltUpdate(snes,p,dp,pnew,ctx,ls_factor,changed_dp,changed_pnew);CHKPETSC(ierr);
    ierr = SNESGetFunction(snes,PETSC_NULL,&func,&fctx);CHKPETSC(ierr);

    Vec& F = rs->GetResidualV();
    Vec& G = rs->GetTrialResV();
    
    ierr = (*func)(snes,p,F,fctx);CHKPETSC(ierr);
    ierr = VecNorm(F,NORM_2,&fnorm);CHKPETSC(ierr);

    ierr = (*func)(snes,pnew,G,fctx);CHKPETSC(ierr);
    ierr = VecNorm(G,NORM_2,&gnorm);CHKPETSC(ierr);

    Vec dp_orig;
    if (!(rs->GetRecordFile().empty())) {
      ierr = VecDuplicate(dp,&dp_orig);
      ierr = VecCopy(dp,dp_orig);
    }
    
    bool norm_acceptable = gnorm < fnorm * rsp.ls_acceptance_factor;
    int ls_iterations = 0;
    bool finished = norm_acceptable 
        || ls_iterations > rsp.max_ls_iterations
        || ls_factor <= rsp.min_ls_factor;

    Real gnorm_0 = gnorm;
    while (!finished) 
    {
        ls_factor *= rsp.ls_reduction_factor;
        if (ls_factor < rsp.min_ls_factor) {
            ls_factor = rsp.min_ls_factor;
        }

        ierr = AltUpdate(snes,p,dp,pnew,ctx,ls_factor,changed_dp,changed_pnew);CHKPETSC(ierr);
        ierr = (*func)(snes,pnew,G,fctx);CHKPETSC(ierr);
        ierr = VecNorm(G,NORM_2,&gnorm);CHKPETSC(ierr);
        norm_acceptable = gnorm < fnorm * rsp.ls_acceptance_factor;
        
        if (ls_factor < 1 
            && rsp.monitor_line_search 
            && ParallelDescriptor::IOProcessor())
	{
            std::cout << tag << tag_ls
                      << "iter=" << ls_iterations
                      << ", step length=" << ls_factor
                      << ", Newton norm=" << gnorm_0
                      << ", damped norm=" << gnorm << '\n';
	}
        
        finished = norm_acceptable 
            || ls_iterations > rsp.max_ls_iterations
            || ls_factor <= rsp.min_ls_factor;      
        ls_iterations++;
    }
    
    if (ls_iterations > rsp.max_ls_iterations) 
    {
        std::string reason = "Solution rejected.  Linear system solved, but ls_iterations too large";
        if (ParallelDescriptor::IOProcessor()) {
            std::cout << tag << tag_ls << reason << std::endl;
        }
        snes->reason = SNES_DIVERGED_LINE_SEARCH;
        rsp.ls_success = false;
        rsp.ls_reason = reason;
    }
    else if (ls_factor <= rsp.min_ls_factor) {
        std::string reason = "Solution rejected.  Linear system solved, but ls_factor too small";
        if (ParallelDescriptor::IOProcessor()) {
            std::cout << tag << tag_ls << reason << std::endl;
        }
        snes->reason = SNES_DIVERGED_LINE_SEARCH;
        rsp.ls_success = false;
        rsp.ls_reason = reason;
    }
    else {
        if (ls_factor == 1) {
            std::string reason = "Full linear step accepted";
            if (ParallelDescriptor::IOProcessor() && rsp.monitor_line_search>1) {
                std::cout << tag << tag_ls << reason << std::endl;
            }
            rsp.ls_reason = reason;
        }
        rsp.ls_success = true;

        int iters = nld->NLIterationsTaken() + 1;
    }

    if (!(rs->GetRecordFile().empty())) {
      RecordSolve(p,dp,dp_orig,pnew,F,G,check_ctx);
      ierr = VecDestroy(&dp_orig);CHKPETSC(ierr);
    }

    PetscFunctionReturn(0);
}

#endif


#if defined(PETSC_3_2)
#include <private/matimpl.h>
#else
#include <petsc-private/matimpl.h> 
#endif

#undef __FUNCT__  
#define __FUNCT__ "RichardMatFDColoringApply"
PetscErrorCode  
RichardMatFDColoringApply(Mat J,MatFDColoring coloring,Vec x1,MatStructure *flag,void *sctx)
{
  PetscErrorCode (*f)(void*,Vec,Vec,void*) = (PetscErrorCode (*)(void*,Vec,Vec,void *))coloring->f;
  PetscErrorCode ierr;
  PetscInt       k,start,end,l,row,col,srow,**vscaleforrow,m1,m2;
  PetscScalar    dx,*y,*w3_array;
  PetscScalar    *vscale_array, *solnTyp_array;
  PetscReal      epsilon = coloring->error_rel,umin = coloring->umin,unorm; 
  Vec            w1=coloring->w1,w2=coloring->w2,w3;
  void           *fctx = coloring->fctx;
  PetscBool      flg = PETSC_FALSE;
  PetscInt       ctype=coloring->ctype,N,col_start=0,col_end=0;
  Vec            x1_tmp;

  PetscFunctionBegin;    
  PetscValidHeaderSpecific(J,MAT_CLASSID,1);
  PetscValidHeaderSpecific(coloring,MAT_FDCOLORING_CLASSID,2);
  PetscValidHeaderSpecific(x1,VEC_CLASSID,3);
  if (!f) SETERRQ(((PetscObject)J)->comm,PETSC_ERR_ARG_WRONGSTATE,"Must call MatFDColoringSetFunction()");

  ierr = PetscLogEventBegin(MAT_FDColoringApply,coloring,J,x1,0);CHKPETSC(ierr);
  ierr = MatSetUnfactored(J);CHKPETSC(ierr);
  ierr = PetscOptionsGetBool(PETSC_NULL,"-mat_fd_coloring_dont_rezero",&flg,PETSC_NULL);CHKPETSC(ierr);
  if (flg) {
    ierr = PetscInfo(coloring,"Not calling MatZeroEntries()\n");CHKPETSC(ierr);
  } else {
    PetscBool  assembled;
    ierr = MatAssembled(J,&assembled);CHKPETSC(ierr);
    if (assembled) {
      ierr = MatZeroEntries(J);CHKPETSC(ierr);
    }
  }

  x1_tmp = x1; 
  if (!coloring->vscale){ 
    ierr = VecDuplicate(x1_tmp,&coloring->vscale);CHKPETSC(ierr);
  }
    
  /*
    This is a horrible, horrible, hack. See DMMGComputeJacobian_Multigrid() it inproperly sets
    coloring->F for the coarser grids from the finest
  */
  if (coloring->F) {
    ierr = VecGetLocalSize(coloring->F,&m1);CHKPETSC(ierr);
    ierr = VecGetLocalSize(w1,&m2);CHKPETSC(ierr);
    if (m1 != m2) {  
      coloring->F = 0; 
      }    
    }   


  RichardSolver* rs = static_rs_ptr;
  BL_ASSERT(rs);
  ierr = VecGetOwnershipRange(w1,&start,&end);CHKPETSC(ierr); /* OwnershipRange is used by ghosted x! */
      
  /* Set w1 = F(x1) */
  if (coloring->F) {
    w1          = coloring->F; /* use already computed value of function */
    coloring->F = 0; 
  } else {
    ierr = PetscLogEventBegin(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
    ierr = (*f)(sctx,x1_tmp,w1,fctx);CHKPETSC(ierr);
    ierr = PetscLogEventEnd(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
  }
      
  if (!coloring->w3) {
    ierr = VecDuplicate(x1_tmp,&coloring->w3);CHKPETSC(ierr);
    ierr = PetscLogObjectParent(coloring,coloring->w3);CHKPETSC(ierr);
  }
  w3 = coloring->w3;

    /* Compute all the local scale factors, including ghost points */
  ierr = VecGetLocalSize(x1_tmp,&N);CHKPETSC(ierr);

  if (rs->Parameters().scale_soln_before_solve) {
      ierr = VecSet(coloring->vscale,1);
      ierr = VecScale(coloring->vscale,1/epsilon);
  }
  else {
      Vec& SolnTypV = rs->GetSolnTypV();
      ierr = VecGetArray(SolnTypV,&solnTyp_array);CHKPETSC(ierr);
      ierr = VecGetArray(coloring->vscale,&vscale_array);CHKPETSC(ierr);
      if (ctype == IS_COLORING_GHOSTED){
          col_start = 0; col_end = N;
      } else if (ctype == IS_COLORING_GLOBAL){
          solnTyp_array = solnTyp_array - start;
          vscale_array = vscale_array - start;
          col_start = start; col_end = N + start;
      }
      for (col=col_start; col<col_end; col++) { 
          vscale_array[col] = (PetscScalar)(1.0 / (solnTyp_array[col] * epsilon));
      } 
      if (ctype == IS_COLORING_GLOBAL)  {
          vscale_array = vscale_array + start;      
          solnTyp_array = solnTyp_array + start;      
      }
      ierr = VecRestoreArray(coloring->vscale,&vscale_array);CHKPETSC(ierr);
      ierr = VecRestoreArray(SolnTypV,&solnTyp_array);CHKPETSC(ierr);
  }

  if (ctype == IS_COLORING_GLOBAL){
      ierr = VecGhostUpdateBegin(coloring->vscale,INSERT_VALUES,SCATTER_FORWARD);CHKPETSC(ierr);
      ierr = VecGhostUpdateEnd(coloring->vscale,INSERT_VALUES,SCATTER_FORWARD);CHKPETSC(ierr);
  }
  
  if (coloring->vscaleforrow) {
    vscaleforrow = coloring->vscaleforrow;
  } else SETERRQ(((PetscObject)J)->comm,PETSC_ERR_ARG_NULL,"Null Object: coloring->vscaleforrow");

  /*
    Loop over each color
  */
  int p = ParallelDescriptor::MyProc();
  if (rs->Parameters().scale_soln_before_solve) {
      //
      // In this case, since the soln is scaled, the perturbation is a simple constant, epsilon
      // Compared to the case where dx=dx_i, the logic cleans up quite a bit here.
      //
      for (k=0; k<coloring->ncolors; k++) { 
          coloring->currentcolor = k;

          ierr = VecCopy(x1_tmp,w3);CHKPETSC(ierr);
          ierr = VecGetArray(w3,&w3_array);CHKPETSC(ierr);
          if (ctype == IS_COLORING_GLOBAL) w3_array = w3_array - start;          
          for (l=0; l<coloring->ncolumns[k]; l++) {
              col = coloring->columns[k][l];    /* local column of the matrix we are probing for */
              w3_array[col] += epsilon;
          } 
          if (ctype == IS_COLORING_GLOBAL) w3_array = w3_array + start;
          ierr = VecRestoreArray(w3,&w3_array);CHKPETSC(ierr);
          
          // w2 = F(w3) - F(x1) = F(x1 + dx) - F(x1)
          ierr = PetscLogEventBegin(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
          ierr = (*f)(sctx,w3,w2,fctx);CHKPETSC(ierr);        
          ierr = PetscLogEventEnd(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
          ierr = VecAXPY(w2,-1.0,w1);CHKPETSC(ierr); 
          
          // Insert (w2_j / dx) into J_ij
          PetscReal epsilon_inv = 1/epsilon;
          ierr = VecGetArray(w2,&y);CHKPETSC(ierr);          
          for (l=0; l<coloring->nrows[k]; l++) {
              row    = coloring->rows[k][l];             /* local row index */
              col    = coloring->columnsforrow[k][l];    /* global column index */
              y[row] *= epsilon_inv;                     /* dx = epsilon */
              srow   = row + start;                      /* global row index */
              ierr   = MatSetValues(J,1,&srow,1,&col,y+row,INSERT_VALUES);CHKPETSC(ierr);
          }
          ierr = VecRestoreArray(w2,&y);CHKPETSC(ierr);
          
      } /* endof for each color */

  }
  else {
      ierr = VecGetArray(coloring->vscale,&vscale_array);CHKPETSC(ierr);
      if (ctype == IS_COLORING_GLOBAL) {
          vscale_array = vscale_array - start;
      }
      
      for (k=0; k<coloring->ncolors; k++) { 
          coloring->currentcolor = k;
          ierr = VecCopy(x1_tmp,w3);CHKPETSC(ierr);
          ierr = VecGetArray(w3,&w3_array);CHKPETSC(ierr);
          if (ctype == IS_COLORING_GLOBAL) {
              w3_array = w3_array - start;
          }
          
          /*
            Loop over each column associated with color 
            adding the perturbation to the vector w3.
          */
          for (l=0; l<coloring->ncolumns[k]; l++) {
              col = coloring->columns[k][l];    /* local column of the matrix we are probing for */
              w3_array[col] += 1/vscale_array[col];
          } 
          if (ctype == IS_COLORING_GLOBAL) {
              w3_array = w3_array + start;
          }
          ierr = VecRestoreArray(w3,&w3_array);CHKPETSC(ierr);
          
          /*
            Evaluate function at w3 = x1 + dx (here dx is a vector of perturbations)
            w2 = F(x1 + dx) - F(x1)
          */
          ierr = PetscLogEventBegin(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
          ierr = (*f)(sctx,w3,w2,fctx);CHKPETSC(ierr);        
          ierr = PetscLogEventEnd(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
          ierr = VecAXPY(w2,-1.0,w1);CHKPETSC(ierr); 
          
          /*
            Loop over rows of vector, putting results into Jacobian matrix
          */
          
          
          ierr = VecGetArray(w2,&y);CHKPETSC(ierr);
          for (l=0; l<coloring->nrows[k]; l++) {
              row    = coloring->rows[k][l];             /* local row index */
              col    = coloring->columnsforrow[k][l];    /* global column index */
              y[row] *= vscale_array[vscaleforrow[k][l]];
              srow   = row + start;
              ierr   = MatSetValues(J,1,&srow,1,&col,y+row,INSERT_VALUES);CHKPETSC(ierr);
          }
          ierr = VecRestoreArray(w2,&y);CHKPETSC(ierr);
                    
      } /* endof for each color */
      if (ctype == IS_COLORING_GLOBAL) {
          vscale_array = vscale_array + start;
      }
      
      ierr = VecRestoreArray(coloring->vscale,&vscale_array);CHKPETSC(ierr);
  }
   
  coloring->currentcolor = -1;
  ierr  = MatAssemblyBegin(J,MAT_FINAL_ASSEMBLY);CHKPETSC(ierr);
  ierr  = MatAssemblyEnd(J,MAT_FINAL_ASSEMBLY);CHKPETSC(ierr);
  ierr = PetscLogEventEnd(MAT_FDColoringApply,coloring,J,x1,0);CHKPETSC(ierr);

  if (dump_Jacobian_and_exit) {
    std::string viewer_filename="mat.output";
    PetscViewer viewer;
    ierr = PetscViewerASCIIOpen(PETSC_COMM_WORLD,viewer_filename.c_str(),&viewer); CHKPETSC(ierr);
    ierr = MatView(J,viewer); CHKPETSC(ierr);
    ierr = PetscViewerDestroy(&viewer); CHKPETSC(ierr);
    ierr = VecGetSize(w2,&N); CHKPETSC(ierr);
    if (ParallelDescriptor::IOProcessor()) {
      std::cout << "There are " << N << " rows in the Jacobian" << std::endl;
    }
    std::string str = "Jacobian written in ASCII to " + viewer_filename + " and run killed from RichardSolver.cpp";
    BoxLib::Abort(str.c_str());
  }

  flg  = PETSC_FALSE;
  ierr = PetscOptionsGetBool(PETSC_NULL,"-mat_null_space_test",&flg,PETSC_NULL);CHKPETSC(ierr);
  if (flg) {
    ierr = MatNullSpaceTest(J->nullsp,J,PETSC_NULL);CHKPETSC(ierr);
  }

  PetscFunctionReturn(0);
}

#undef __FUNCT__  
#define __FUNCT__ "ComputeRichardAlpha"
void
RichardSolver::ComputeRichardAlpha(Vec& Alpha,const Vec& Pressure)
{
  MFTower& PCapParamsMFT = GetPCapParams();
  MFTower& PMFT = GetPressure();
  MFTower& aMFT = GetAlpha();
  PetscErrorCode ierr = GetLayout().VecToMFTower(PMFT,Pressure,0); CHKPETSC(ierr);

  //Real total_volume_inv = 1/TotalVolume();

  for (int lev=0; lev<nLevs; ++lev) {

    pm[lev].calcInvPressure(GetRhoSatNp1()[lev],PMFT[lev]);
    
    for (MFIter mfi(PMFT[lev]); mfi.isValid(); ++mfi) {
      const Box& vbox = mfi.validbox();

      FArrayBox& pofab = GetPorosity()[lev][mfi];
      FArrayBox& kcfab = GetKappaCCavg()[lev][mfi];
      FArrayBox& cpfab = PCapParamsMFT[lev][mfi];
      const int n_cp_coef = cpfab.nComp();

      FArrayBox& nfab = GetRhoSatNp1()[lev][mfi];
      FArrayBox& afab = aMFT[lev][mfi];
      
      FORT_RICHARD_ALPHA(afab.dataPtr(), ARLIM(afab.loVect()), ARLIM(afab.hiVect()),
			 nfab.dataPtr(), ARLIM(nfab.loVect()),ARLIM(nfab.hiVect()),
			 pofab.dataPtr(), ARLIM(pofab.loVect()),ARLIM(pofab.hiVect()),
			 kcfab.dataPtr(), ARLIM(kcfab.loVect()), ARLIM(kcfab.hiVect()),
			 cpfab.dataPtr(), ARLIM(cpfab.loVect()), ARLIM(cpfab.hiVect()), &n_cp_coef,
			 vbox.loVect(), vbox.hiVect());
      
    }
    
    //MultiFab::Multiply(aMFT[lev],GetLayout().Volume(lev),0,0,1,0);
    //aMFT[lev].mult(total_volume_inv,0,1);
  }

  // Put into Vec data structure
  ierr = GetLayout().MFTowerToVec(Alpha,aMFT,0); CHKPETSC(ierr);
}


#undef __FUNCT__  
#define __FUNCT__ "SemiAnalyticMatFDColoringApply"
PetscErrorCode  
SemiAnalyticMatFDColoringApply(Mat J,MatFDColoring coloring,Vec x1,MatStructure *flag,void *sctx)
{
  PetscErrorCode (*f)(void*,Vec,Vec,void*) = (PetscErrorCode (*)(void*,Vec,Vec,void *))coloring->f;
  PetscErrorCode ierr;
  PetscInt       k,start,end,l,row,col,srow,**vscaleforrow,m1,m2;
  PetscScalar    dx,*y,*w3_array;
  PetscScalar    *vscale_array, *solnTyp_array, *a_array;
  PetscReal      epsilon = coloring->error_rel,umin = coloring->umin,unorm; 
  Vec            w1=coloring->w1,w2=coloring->w2,w3;
  void           *fctx = coloring->fctx;
  PetscBool      flg = PETSC_FALSE;
  PetscInt       ctype=coloring->ctype,N,col_start=0,col_end=0;
  Vec            x1_tmp;

  PetscFunctionBegin;    
  PetscValidHeaderSpecific(J,MAT_CLASSID,1);
  PetscValidHeaderSpecific(coloring,MAT_FDCOLORING_CLASSID,2);
  PetscValidHeaderSpecific(x1,VEC_CLASSID,3);
  if (!f) SETERRQ(((PetscObject)J)->comm,PETSC_ERR_ARG_WRONGSTATE,"Must call MatFDColoringSetFunction()");

  ierr = PetscLogEventBegin(MAT_FDColoringApply,coloring,J,x1,0);CHKPETSC(ierr);
  ierr = MatSetUnfactored(J);CHKPETSC(ierr);
  ierr = PetscOptionsGetBool(PETSC_NULL,"-mat_fd_coloring_dont_rezero",&flg,PETSC_NULL);CHKPETSC(ierr);
  if (flg) {
    ierr = PetscInfo(coloring,"Not calling MatZeroEntries()\n");CHKPETSC(ierr);
  } else {
    PetscBool  assembled;
    ierr = MatAssembled(J,&assembled);CHKPETSC(ierr);
    if (assembled) {
      ierr = MatZeroEntries(J);CHKPETSC(ierr);
    }
  }

  x1_tmp = x1; 
  if (!coloring->vscale){ 
    ierr = VecDuplicate(x1_tmp,&coloring->vscale);CHKPETSC(ierr);
  }
    
  /*
    This is a horrible, horrible, hack. See DMMGComputeJacobian_Multigrid() it inproperly sets
    coloring->F for the coarser grids from the finest
  */
  if (coloring->F) {
    ierr = VecGetLocalSize(coloring->F,&m1);CHKPETSC(ierr);
    ierr = VecGetLocalSize(w1,&m2);CHKPETSC(ierr);
    if (m1 != m2) {  
      coloring->F = 0; 
      }    
    }   
  RichardSolver* rs = static_rs_ptr;
  BL_ASSERT(rs);
  Vec& AlphaV = rs->GetAlphaV();
  
  Vec& press = rs->GetTrialResV(); // A handy Vec to use
  ierr = VecCopy(x1_tmp,press); CHKPETSC(ierr);
  if (rs->Parameters().scale_soln_before_solve) {
      ierr = VecPointwiseMult(press,x1_tmp,rs->GetSolnTypV()); CHKPETSC(ierr); // Mult(w,x,y): w=x.y, p=pbar.ptyp
  }
  rs->ComputeRichardAlpha(AlphaV,press);

  if (rs->Parameters().scale_soln_before_solve) {
      ierr = VecPointwiseMult(AlphaV,AlphaV,rs->GetSolnTypV()); CHKPETSC(ierr); // Mult(w,x,y): w=x.y, alphabar=alpha.ptyp
  }

  Real dt_inv = 1/rs->GetDt();

  ierr = VecGetOwnershipRange(w1,&start,&end);CHKPETSC(ierr); /* OwnershipRange is used by ghosted x! */

  if (!rs->Parameters().centered_diff_J) {
      /* Set w1 = F(x1) */
      if (coloring->F) {
          w1          = coloring->F; /* use already computed value of function */
          coloring->F = 0; 
      } else {
          ierr = PetscLogEventBegin(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
          ierr = (*f)(sctx,x1_tmp,w1,fctx);CHKPETSC(ierr);
          ierr = PetscLogEventEnd(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
      }
  }
      
  if (!coloring->w3) {
    ierr = VecDuplicate(x1_tmp,&coloring->w3);CHKPETSC(ierr);
    ierr = PetscLogObjectParent(coloring,coloring->w3);CHKPETSC(ierr);
  }
  w3 = coloring->w3;

    /* Compute all the local scale factors, including ghost points */
  ierr = VecGetLocalSize(x1_tmp,&N);CHKPETSC(ierr);

  ierr = VecSet(coloring->vscale,1); CHKPETSC(ierr);
  ierr = VecScale(coloring->vscale,1/epsilon); CHKPETSC(ierr);

  if (ctype == IS_COLORING_GLOBAL){
      ierr = VecGhostUpdateBegin(coloring->vscale,INSERT_VALUES,SCATTER_FORWARD);CHKPETSC(ierr);
      ierr = VecGhostUpdateEnd(coloring->vscale,INSERT_VALUES,SCATTER_FORWARD);CHKPETSC(ierr);
  }
  
  if (coloring->vscaleforrow) {
    vscaleforrow = coloring->vscaleforrow;
  } else SETERRQ(((PetscObject)J)->comm,PETSC_ERR_ARG_NULL,"Null Object: coloring->vscaleforrow");

  /*
    Loop over each color
  */
  int p = ParallelDescriptor::MyProc();
  //
  // In this case, since the soln is scaled, the perturbation is a simple constant, epsilon
  // Compared to the case where dx=dx_i, the logic cleans up quite a bit here.
  //
  Vec& w4 = rs->GetTrialResV(); // A handy Vec to use if centered diff for J

  for (k=0; k<coloring->ncolors; k++) { 
      coloring->currentcolor = k;
      
      ierr = VecCopy(x1_tmp,w3);CHKPETSC(ierr);
      ierr = VecGetArray(w3,&w3_array);CHKPETSC(ierr);
      if (ctype == IS_COLORING_GLOBAL) w3_array = w3_array - start;          

      ierr = VecCopy(x1_tmp,w4);CHKPETSC(ierr);
      PetscReal *w4_array;
      ierr = VecGetArray(w4,&w4_array);CHKPETSC(ierr);
      if (ctype == IS_COLORING_GLOBAL) w4_array = w4_array - start;          

      for (l=0; l<coloring->ncolumns[k]; l++) {
          col = coloring->columns[k][l];    /* local column of the matrix we are probing for */
          w3_array[col] += epsilon;
          w4_array[col] -= epsilon;
      } 
      if (ctype == IS_COLORING_GLOBAL) w3_array = w3_array + start;
      ierr = VecRestoreArray(w3,&w3_array);CHKPETSC(ierr);
      
      if (ctype == IS_COLORING_GLOBAL) w4_array = w4_array + start;
      ierr = VecRestoreArray(w4,&w4_array);CHKPETSC(ierr);

      PetscReal epsilon_inv;
      if (rs->Parameters().centered_diff_J) {
          // w2 <- w2 - w1 = F(w3) - F(w4) = F(x1 + dx) - F(x1 - dx)
          ierr = PetscLogEventBegin(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
          ierr = (*f)(sctx,w3,w2,fctx);CHKPETSC(ierr);        
          ierr = (*f)(sctx,w4,w1,fctx);CHKPETSC(ierr);        
          ierr = PetscLogEventEnd(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
          ierr = VecAXPY(w2,-1.0,w1);CHKPETSC(ierr); 
          epsilon_inv = 0.5/epsilon;
      }
      else {
          // w2 = F(w3) - F(x1) = F(x1 + dx) - F(x1)
          ierr = PetscLogEventBegin(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
          ierr = (*f)(sctx,w3,w2,fctx);CHKPETSC(ierr);        
          ierr = PetscLogEventEnd(MAT_FDColoringFunction,0,0,0,0);CHKPETSC(ierr);
          ierr = VecAXPY(w2,-1.0,w1);CHKPETSC(ierr); 
          epsilon_inv = 1/epsilon;
      }
      
      // Insert (w2_j / dx) into J_ij [include diagonal term, dR1_i/dpbar_i = alphabar
      ierr = VecGetArray(w2,&y);CHKPETSC(ierr);          
      ierr = VecGetArray(AlphaV,&a_array);CHKPETSC(ierr);          

      if (ctype == IS_COLORING_GLOBAL) a_array -= start;          

      for (l=0; l<coloring->nrows[k]; l++) {
          row    = coloring->rows[k][l];             /* local row index */
          col    = coloring->columnsforrow[k][l];    /* global column index */
          y[row] *= epsilon_inv;                     /* dx = epsilon */
          srow   = row + start;                      /* global row index */

          if (srow == col) {
              y[row] += a_array[srow] * dt_inv;
          }
          ierr   = MatSetValues(J,1,&srow,1,&col,y+row,INSERT_VALUES);CHKPETSC(ierr);
      }
      if (ctype == IS_COLORING_GLOBAL) {
          a_array += start;          
      }
      ierr = VecRestoreArray(AlphaV,&a_array);CHKPETSC(ierr);
      ierr = VecRestoreArray(w2,&y);CHKPETSC(ierr);
      
  } /* end of for each color */
   
  coloring->currentcolor = -1;
  ierr  = MatAssemblyBegin(J,MAT_FINAL_ASSEMBLY);CHKPETSC(ierr);
  ierr  = MatAssemblyEnd(J,MAT_FINAL_ASSEMBLY);CHKPETSC(ierr);
  ierr = PetscLogEventEnd(MAT_FDColoringApply,coloring,J,x1,0);CHKPETSC(ierr);

  if (dump_Jacobian_and_exit) {
    std::string viewer_filename="mat.output";
    PetscViewer viewer;
    ierr = PetscViewerASCIIOpen(PETSC_COMM_WORLD,viewer_filename.c_str(),&viewer); CHKPETSC(ierr);
    ierr = MatView(J,viewer); CHKPETSC(ierr);
    ierr = PetscViewerDestroy(&viewer); CHKPETSC(ierr);
    ierr = VecGetSize(w2,&N); CHKPETSC(ierr);
    if (ParallelDescriptor::IOProcessor()) {
      std::cout << "There are " << N << " rows in the Jacobian" << std::endl;
    }
    std::string str = "Jacobian written in ASCII to " + viewer_filename + " and run killed from RichardSolver.cpp";
    BoxLib::Abort(str.c_str());
  }

  flg  = PETSC_FALSE;
  ierr = PetscOptionsGetBool(PETSC_NULL,"-mat_null_space_test",&flg,PETSC_NULL);CHKPETSC(ierr);
  if (flg) {
    ierr = MatNullSpaceTest(J->nullsp,J,PETSC_NULL);CHKPETSC(ierr);
  }

  PetscFunctionReturn(0);
}

#undef __FUNCT__  
#define __FUNCT__ "RichardComputeJacobianColor"
PetscErrorCode 
RichardComputeJacobianColor(SNES snes,Vec x1,Mat *J,Mat *B,MatStructure *flag,void *ctx)
{
  MatFDColoring  color = (MatFDColoring) ctx;
  PetscErrorCode ierr;
  Vec            f;
  PetscErrorCode (*ff)(void),(*fd)(void);

  // ick!
  RichardSolver* rs = static_rs_ptr;
  BL_ASSERT(rs);

  PetscFunctionBegin;
  PetscValidHeaderSpecific(color,MAT_FDCOLORING_CLASSID,6);

  if (rs->ReusePreviousJacobian()) {
      PetscFunctionReturn(0);
  }

  *flag = SAME_NONZERO_PATTERN;
  ierr  = SNESGetFunction(snes,&f,(PetscErrorCode (**)(SNES,Vec,Vec,void*))&ff,0);CHKPETSC(ierr);
  ierr  = MatFDColoringGetFunction(color,&fd,PETSC_NULL);CHKPETSC(ierr);
  if (fd == ff) { /* reuse function value computed in SNES */
    ierr  = MatFDColoringSetF(color,f);CHKPETSC(ierr);
  }
  if (rs->Parameters().semi_analytic_J) {
      ierr = SemiAnalyticMatFDColoringApply(*B,color,x1,flag,snes);CHKPETSC(ierr);
  } 
  else {
      ierr = RichardMatFDColoringApply(*B,color,x1,flag,snes);CHKPETSC(ierr);
  }
  if (*J != *B) {
    ierr = MatAssemblyBegin(*J,MAT_FINAL_ASSEMBLY);CHKPETSC(ierr);
    ierr = MatAssemblyEnd(*J,MAT_FINAL_ASSEMBLY);CHKPETSC(ierr);
  }

  rs->ResetRemainingJacobianReuses();

  PetscFunctionReturn(0);
}

#undef __FUNCT__  
#define __FUNCT__ "MatSqueeze"
static void
MatSqueeze(Mat& J,
	   Layout& layout) 
{
  PetscErrorCode ierr;
  Mat A;
  MatCreate(PETSC_COMM_WORLD,&A);
  MatSetType(A,MATSEQAIJ);
  int n = layout.NumberOfLocalNodeIds();
  int N = layout.NumberOfGlobalNodeIds();

  ierr = MatSetSizes(A,n,n,N,N);  CHKPETSC(ierr);
  
  int rstart, rend;
  ierr = MatGetOwnershipRange(J,&rstart,&rend);CHKPETSC(ierr);
  int nrows = 0;
  int Jncols, Ancols;
  const PetscInt *Jcols;
  const PetscScalar *Jvals;
  PetscReal dtol = 1.e-20;
  for (int row=rstart; row<rend; row++){
    Array<PetscInt> Acols(0);
    Array<PetscReal> Avals(0);
    ierr = MatGetRow(J,row,&Jncols,&Jcols,&Jvals);CHKPETSC(ierr);
    for (int j=0; j<Jncols; j++){
      PetscScalar Jval = Jvals[j];
      if (std::abs(Jval) > dtol) {
	Acols.push_back(Jcols[j]);
	Avals.push_back(Jval);
      }
    }
    BL_ASSERT(Acols.size()>0 && Acols.size()==Avals.size());
    int one = 1;
    ierr = MatSetValues(A,one,&row,Avals.size(),Acols.dataPtr(),Avals.dataPtr(),INSERT_VALUES); CHKPETSC(ierr);
    ierr = MatRestoreRow(J,row,&Jncols,&Jcols,&Jvals);CHKPETSC(ierr);
  }
  ierr = MatAssemblyBegin(A,MAT_FINAL_ASSEMBLY); CHKPETSC(ierr);
  ierr = MatAssemblyEnd(A,MAT_FINAL_ASSEMBLY); CHKPETSC(ierr);

  MatView(A,PETSC_VIEWER_STDOUT_SELF);
}

