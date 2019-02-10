#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <gsl/gsl_errno.h>
#include <gsl/gsl_integration.h>

#include "particle.h"
#include "cgs.h"
#include "utilities.h"

#define LIMIT 1000
#define EPSREL 1e-5

Particle::Particle() {
}

Particle::Particle(PID pid, double efficiency) :
		_pid(pid), _efficiency(efficiency) {
}

std::string Particle::make_filename() const {
	std::string filename = "particle";
	filename += "_" + std::to_string(_pid.get_Z());
	filename += "_" + std::to_string(_pid.get_A()) + ".log";
	return filename;
}

void Particle::clear() {
	if (_X != nullptr)
		delete _X;
	if (_Q != nullptr)
		delete _Q;
	if (_Q_sec != nullptr)
		delete _Q_sec;
	if (_Q_ter != nullptr)
		delete _Q_ter;
	if (_sigma != nullptr)
		delete _sigma;
	if (_dEdx != nullptr)
		delete _dEdx;
}

Particle::~Particle() {
}

void Particle::build_secondary_source(const std::vector<Particle>& particles,
		const Params& params) {
	auto xsecs = (params.id == 0) ? SpallationXsecs(_pid) : SpallationXsecs(_pid, true);
	const size_t size = 100;
	auto T_s = LogAxis(0.1 * cgs::GeV, 10. * cgs::TeV, size);
	std::vector<double> Q_s;
	for (auto& T : T_s) {
		double value = 0;
		for (auto& particle : particles) {
			if (particle.get_pid().get_A() > _pid.get_A() && particle.isDone()) {
				value += xsecs.get_ISM(particle.get_pid(), T) * particle.I_T_interpol(T);
			}
		}
		value /= cgs::mean_ism_mass;
		Q_s.push_back(value);
	}
	_Q_sec = new SourceTerm(T_s, Q_s);
}

void Particle::build_tertiary_source(const std::vector<Particle>& particles) {
	const size_t size = 100;
	auto T_t = LogAxis(0.1 * cgs::GeV, 10. * cgs::TeV, size);
	std::vector<double> Q_t;
	for (auto& T : T_t) {
		const double T_prime = T / cgs::inelasticity;
		double sigma_ISM = sigma_pp(T_prime);
		sigma_ISM *= (1. + cgs::K_He * cgs::f_He) / (1. + cgs::f_He);
		double value = sigma_ISM / cgs::inelasticity;
		value *= (T_prime + cgs::proton_mass_c2) / (T + cgs::proton_mass_c2);
		value *= std::pow(T * (T + 2. * cgs::proton_mass_c2), 1.5)
				/ std::pow(T_prime * (T_prime + 2. * cgs::proton_mass_c2), 1.5);
		for (auto& particle : particles) {
			if (particle.get_pid() == H1 && particle.isDone()) {
				value *= particle.I_T_interpol(T_prime);
			}
		}
		value /= cgs::mean_ism_mass;
		Q_t.push_back(value);
	}
	_Q_ter = new SourceTerm(T_t, Q_t);
}

void Particle::build_grammage_at_source(const std::vector<Particle>& particles,
		const Params& params) {
	auto xsecs = (params.id == 0) ? SpallationXsecs(_pid) : SpallationXsecs(_pid, true);
	const size_t size = 100;
	auto T_X = LogAxis(0.1 * cgs::GeV, 10. * cgs::TeV, size);
	std::vector<double> Q_X;
	for (auto& T : T_X) {
		double value = 0;
		for (auto& particle : particles) {
			if (particle.get_pid().get_A() > _pid.get_A() && particle.isDone()) {
				double r = params.X_s / cgs::mean_ism_mass * xsecs.get_ISM(particle.get_pid(), T);
				auto Q = new SnrSource(particle.get_pid(), particle.get_efficiency(), params);
				value += r * Q->get(T);
			}
		}
		Q_X.push_back(value);
	}
	_Q_Xs = new SourceTerm(T_X, Q_X);
}

double Particle::I_T_interpol(const double& T) const {
	return LinearInterpolatorLog(_T, _I_T, T);
}

double Particle::I_R_LIS(const double& R) const {
	double value = 0;
	constexpr double mp_2 = pow2(cgs::proton_mass_c2);
	double E_2 = pow2(R * _pid.get_Z_over_A()) + mp_2;
	double T_ = std::sqrt(E_2) - cgs::proton_mass_c2;
	{
		double Z_A_squared = pow2(_pid.get_Z_over_A());
		double dTdR = R * Z_A_squared;
		dTdR /= std::sqrt(Z_A_squared * pow2(R) + mp_2);
		value = I_T_interpol(T_) * dTdR;
	}
	return value;
}

double Particle::I_R_TOA(const double& R, const double& modulation_potential) const {
	double value = 0;
	constexpr double mp_2 = pow2(cgs::proton_mass_c2);
	double E_2 = pow2(R * _pid.get_Z_over_A()) + pow2(cgs::proton_mass_c2);
	double T_now = std::sqrt(E_2) - cgs::proton_mass_c2;
	if (T_now > _T.front() && T_now < _T.back()) {
		double Phi = _pid.get_Z_over_A() * modulation_potential;
		double T_ISM = std::min(T_now + Phi, _T.back());
		double Z_A_squared = pow2(_pid.get_Z_over_A());
		double dTdR = R * Z_A_squared;
		dTdR /= std::sqrt(Z_A_squared * pow2(R) + mp_2);
		double factor = (pow2(T_now) - mp_2) / (pow2(T_ISM) - mp_2);
		value = factor * I_T_interpol(T_ISM) * dTdR;
	}
	return value;
}

bool Particle::run(const std::vector<double>& T) {
	_T = std::vector<double>(T);
	for (auto& T_now : _T) {
		_I_T.push_back(compute_integral(T_now));
	}
	return _I_T.size() == _T.size();
}

double Particle::Lambda_1(const double& T) {
	double value = 1. / _X->get(T) + _sigma->get_ISM(T) / cgs::mean_ism_mass
			+ _dEdx->get_derivative(T);
	return value;
}

double Particle::Lambda_2(const double& T) {
	double value = _dEdx->get(T);
	return std::fabs(value);
}

double Particle::Q(const double& T) {
	double value = 0;
	if (_pid == H1_ter) {
		value = _Q_ter->get(T);
	} else {
		value = _Q->get(T) + _Q_sec->get(T) + _Q_Xs->get(T);
	}
	return value;
}

double compute_integral_qags(gsl_integration_workspace * w, gsl_function * F, double x_lo,
		double x_hi) {
	double result, error;
	gsl_integration_qags(F, x_lo, x_hi, 0, EPSREL, LIMIT, w, &result, &error);
	return result;
}

double compute_integral_qag(gsl_integration_workspace * w, gsl_function * F, double x_lo,
		double x_hi) {
	double result, error;
	int key = 3;
	gsl_integration_qag(F, x_lo, x_hi, 0, EPSREL, LIMIT, key, w, &result, &error);
	return result;
}

double Particle::internal_integrand(const double& T_second) {
	return Lambda_1(T_second) / Lambda_2(T_second);
}

double gslParticleClassExpWrapper(double x, void * pp) {
	double E_second = std::exp(x);
	Particle * particle = (Particle *) pp;
	return E_second * particle->internal_integrand(E_second);
}

double Particle::ExpIntegral(const double& T, const double& T_prime) {
	double result = 0;
	gsl_function F;
	F.params = this;
	F.function = &gslParticleClassExpWrapper;
	gsl_integration_workspace * w = gsl_integration_workspace_alloc(LIMIT);
	result = compute_integral_qags(w, &F, std::log(T), std::log(T_prime));
	gsl_integration_workspace_free(w);
	return result;
}

double Particle::external_integrand(const double& T_prime, const double& T) {
	double value = Q(T_prime) * std::exp(-ExpIntegral(T, T_prime));
	value /= Lambda_2(T_prime);
	return value;
}

struct gsl_f_pars {
	double T;
	Particle * pt_Particle;
};

double gslParticleClassWrapper(double x, void * pp) {
	gsl_f_pars *p = (gsl_f_pars *) pp;
	double T = p->T;
	double T_prime = std::exp(x);
	return T_prime * p->pt_Particle->external_integrand(T_prime, T);
}

double Particle::compute_integral(const double& T) {
	double result = 0;
	gsl_f_pars pars = { T, this };
	gsl_function F;
	F.params = &pars;
	F.function = &gslParticleClassWrapper;
	gsl_integration_workspace * w = gsl_integration_workspace_alloc(LIMIT);
	result = compute_integral_qags(w, &F, std::log(T), std::log(1e3 * T));
	gsl_integration_workspace_free(w);
	return result;
}

void Particle::dump() const {
	auto filename = make_filename();
	std::ofstream outfile(filename);
	outfile << std::scientific;
	for (double T = cgs::GeV; T < 1.1 * cgs::TeV; T *= 1.1) {
		outfile << T / cgs::GeV << "\t";
		double E = T + cgs::proton_mass_c2;
		double R = pc_func(_pid.get_A(), T) / (double) _pid.get_Z();
		outfile << R / cgs::GeV << "\t";
		outfile << _Q->get(T) << "\t";
		outfile << _X->get(T) / (cgs::gram / cgs::cm2) << "\t";
		outfile << _X->diffusion_timescale(T) / cgs::year << "\t";
		outfile << _X->advection_timescale() / cgs::year << "\t";
		outfile << cgs::mean_ism_mass / _sigma->get_ISM(T) / (cgs::gram / cgs::cm2) << "\t";
//		outfile << T / -_dEdx->get(T) / (cgs::gram / cgs::cm2) << "\t";
		//outfile << (1. / cgs::c_light / n_H / _sigma->get_ISM(T)) / cgs::year << "\t";
//		std::cout << -(E / b.dE_dt_adiabatic(E)) / year << "\t";
//		std::cout << -(E / b.dE_dt_ionization(E)) / year << "\t";
		outfile << "\n";
	}
	outfile.close();
}

#undef LIMIT
