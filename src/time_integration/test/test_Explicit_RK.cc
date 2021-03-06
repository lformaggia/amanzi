#include "UnitTest++.h"

#include "Explicit_TI_FnBase.hh"
#include "Explicit_TI_RK.hh"

#include "Epetra_BlockMap.h"
#include "Epetra_Vector.h"
#include "Epetra_SerialDenseMatrix.h"
#include "Epetra_SerialComm.h"

SUITE(TimeIntegrationTests) {
using namespace Amanzi;

  // ODE: y' = y
class fn1 : public Explicit_TI::fnBase<Epetra_Vector> {
 public:
  void Functional(const double t, const Epetra_Vector& y, Epetra_Vector& y_new) {
    y_new = y;
  }
};


  TEST(Explicit_RK_Euler) {
    std::cout << "Test: Explicit_RK_Euler" << std::endl;    
    Epetra_Comm* comm = new Epetra_SerialComm();    
    Epetra_BlockMap map(1,1,0,*comm);
    Epetra_Vector y(map);
    Epetra_Vector y_new(map);

    fn1 f;
    Explicit_TI::RK<Epetra_Vector>::method_t method = Explicit_TI::RK<Epetra_Vector>::forward_euler;
    Explicit_TI::RK<Epetra_Vector> explicit_time_integrator(f, method, y); 
		
    // initial value
    y.PutScalar(1.0);

    // initial time
    double t=0.0;
    // time step
    double h=.1;
    
    // integrate to t=1.0
    do 
      {
	explicit_time_integrator.TimeStep(t,h,y,y_new);
	t=t+h;
	y = y_new;
      }
    while (t<1.0);
    CHECK_CLOSE(y[0],exp(t),2.0*h);
  }
       

  TEST(Explicit_RK_Heun) {
    std::cout << "Test: Explicit_RK_Heun" << std::endl;    
    
    Epetra_Comm* comm = new Epetra_SerialComm();    
    Epetra_BlockMap map(1,1,0,*comm);
    Epetra_Vector y(map);
    Epetra_Vector y_new(map);

    fn1 f;
    Explicit_TI::RK<Epetra_Vector>::method_t method = Explicit_TI::RK<Epetra_Vector>::heun_euler;
    Explicit_TI::RK<Epetra_Vector> explicit_time_integrator(f, method, y); 
		
    // initial value
    y.PutScalar(1.0);

    // initial time
    double t=0.0;
    // time step
    double h=.1;
    
    // integrate to t=1.0
    do 
      {
	explicit_time_integrator.TimeStep(t,h,y,y_new);
	t=t+h;
	y = y_new;
      }
    while (t<1.0);
    CHECK_CLOSE(y[0],exp(t),pow(h,2));
  }
       

  TEST(Explicit_RK_Midpoint) {
    std::cout << "Test: Explicit_RK_Midpoint" << std::endl;    
    
    Epetra_Comm* comm = new Epetra_SerialComm();    
    Epetra_BlockMap map(1,1,0,*comm);
    Epetra_Vector y(map);
    Epetra_Vector y_new(map);

    fn1 f;
    Explicit_TI::RK<Epetra_Vector>::method_t method = Explicit_TI::RK<Epetra_Vector>::midpoint;
    Explicit_TI::RK<Epetra_Vector> explicit_time_integrator(f, method, y); 
		
    // initial value
    y.PutScalar(1.0);

    // initial time
    double t=0.0;
    // time step
    double h=.1;
    
    // integrate to t=1.0
    do 
      {
	explicit_time_integrator.TimeStep(t,h,y,y_new);
	t=t+h;
	y = y_new;
      }
    while (t<1.0);
    CHECK_CLOSE(y[0],exp(t),pow(h,2));
  }

  TEST(Explicit_RK_Ralston) {
    std::cout << "Test: Explicit_RK_Rapson" << std::endl;    
    
    Epetra_Comm* comm = new Epetra_SerialComm();    
    Epetra_BlockMap map(1,1,0,*comm);
    Epetra_Vector y(map);
    Epetra_Vector y_new(map);

    fn1 f;
    Explicit_TI::RK<Epetra_Vector>::method_t method = Explicit_TI::RK<Epetra_Vector>::ralston;
    Explicit_TI::RK<Epetra_Vector> explicit_time_integrator(f, method, y); 
		
    // initial value
    y.PutScalar(1.0);

    // initial time
    double t=0.0;
    // time step
    double h=.1;
    
    // integrate to t=1.0
    do 
      {
	explicit_time_integrator.TimeStep(t,h,y,y_new);
	t=t+h;
	y = y_new;
      }
    while (t<1.0);
    CHECK_CLOSE(y[0],exp(t),pow(h,2));
  }


  TEST(Explicit_RK_Kutta3D) {
    std::cout << "Test: Explicit_RK_Kutta3D" << std::endl;    
    
    Epetra_Comm* comm = new Epetra_SerialComm();    
    Epetra_BlockMap map(1,1,0,*comm);
    Epetra_Vector y(map);
    Epetra_Vector y_new(map);

    fn1 f;
    Explicit_TI::RK<Epetra_Vector>::method_t method = Explicit_TI::RK<Epetra_Vector>::kutta_3rd_order;
    Explicit_TI::RK<Epetra_Vector> explicit_time_integrator(f, method, y); 
		
    // initial value
    y.PutScalar(1.0);

    // initial time
    double t=0.0;
    // time step
    double h=.1;
    
    // integrate to t=1.0
    do 
      {
	explicit_time_integrator.TimeStep(t,h,y,y_new);
	t=t+h;
	y = y_new;
      }
    while (t<1.0);
    CHECK_CLOSE(y[0],exp(t),pow(h,3));
  }

  TEST(Explicit_RK_UserDefined) {
    std::cout << "Test: Explicit_RK_UserDefined" << std::endl;    
    
    Epetra_Comm* comm = new Epetra_SerialComm();    
    Epetra_BlockMap map(1,1,0,*comm);
    Epetra_Vector y(map);
    Epetra_Vector y_new(map);

    fn1 f;
    int order = 2;
    Epetra_SerialDenseMatrix a(order,order);
    std::vector<double> b(order);
    std::vector<double> c(order);
    
    a(1,0) = 1.0;

    b[0] = 0.5;
    b[1] = 0.5;

    c[0] = 0.0;
    c[1] = 1.0;

    Explicit_TI::RK<Epetra_Vector> explicit_time_integrator(f, order, a, b, c, y); 
		
    // initial value
    y.PutScalar(1.0);

    // initial time
    double t=0.0;
    // time step
    double h=.1;
    
    // integrate to t=1.0
    do 
      {
	explicit_time_integrator.TimeStep(t,h,y,y_new);
	t = t + h;
	y = y_new;
      }
    while (t<1.0);


    CHECK_CLOSE(y[0],exp(t),pow(h,2));
  }

  TEST(Explicit_RK_RK4) {
    std::cout << "Test: Explicit_RK_RK4" << std::endl;    
    
    Epetra_Comm* comm = new Epetra_SerialComm();    
    Epetra_BlockMap map(1,1,0,*comm);
    Epetra_Vector y(map);
    Epetra_Vector y_new(map);

    fn1 f;
    Explicit_TI::RK<Epetra_Vector>::method_t method = Explicit_TI::RK<Epetra_Vector>::runge_kutta_4th_order;
    Explicit_TI::RK<Epetra_Vector> explicit_time_integrator(f, method, y); 
		
    // initial value
    y.PutScalar(1.0);

    // initial time
    double t=0.0;
    // time step
    double h=.1;
    
    // integrate to t=1.0
    do 
      {
	explicit_time_integrator.TimeStep(t, h, y, y_new);
	t = t + h;
	y = y_new;
      }
    while (t<1.0);
    CHECK_CLOSE(y[0],exp(t),pow(h,4));
  }
}
