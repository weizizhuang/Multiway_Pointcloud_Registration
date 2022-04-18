#include "fast_global_registration.h"
#include <flann/flann.hpp>
#include <iostream>
using namespace std;
#include "./ScaleStretchICP/include/ssicp.h"
#include <pcl/sample_consensus/sac_model_registration.h>
#include <igl/slice.h>
#include <pcl/sample_consensus/ransac.h>
#include <igl/writeDMAT.h>
#include <random>
#include<fstream>

#define DIV_FACTOR			1.4		// Division factor used for graduated non-convexity
#define USE_ABSOLUTE_SCALE	0		// Measure distance in absolute scale (1) or in scale relative to the diameter of the model (0)
#define MAX_CORR_DIST		0.0001	// Maximum correspondence distance (also see comment of USE_ABSOLUTE_SCALE)
#define TUPLE_SCALE			0.95	// Similarity measure used for tuples of feature points.
#define TUPLE_MAX_CNT		10000	// Maximum tuple numbers.
#define SHOW_DEBUG_INFO
#define TUPLE_SIMILAR_CRITERIA

double global_all_scale=1.0;
int global_iter = -1;
//#define STD_FILTER
//#define USE_RANSAC

void SearchFLANNTree(flann::Index<flann::L1<float>>* index,
	Eigen::VectorXf& input,
	std::vector<int>& indices,
	std::vector<float>& dists,
	int nn)
{
	int rows_t = 1;
	int dim = input.size();

	std::vector<float> query;
	query.resize(rows_t*dim);
	for (int i = 0; i < dim; i++)
		query[i] = input(i);
	flann::Matrix<float> query_mat(&query[0], rows_t, dim);

	indices.resize(rows_t*nn);
	dists.resize(rows_t*nn);
	flann::Matrix<int> indices_mat(&indices[0], rows_t, nn);
	flann::Matrix<float> dists_mat(&dists[0], rows_t, nn);

	index->knnSearch(query_mat, indices_mat, dists_mat, nn, flann::SearchParams(128));
}

void FastGlobalRegistration::advanced_matching(const Eigen::MatrixXd& v_1, const Eigen::MatrixXd& v_2, const Eigen::MatrixXd& fpfh_1, const Eigen::MatrixXd& fpfh_2, std::vector<std::pair<int, int> >& corres)
{

	std::vector<const Eigen::MatrixXd *> pointcloud(2);
	pointcloud[0] = &v_1;
	pointcloud[1] = &v_2;
	std::vector<const Eigen::MatrixXd *> features(2);
	features[0] = &fpfh_1;
	features[1] = &fpfh_2;

	int fi = 0;
	int fj = 1;

	printf("Advanced matching : [%d - %d]\n", fi, fj);
	bool swapped = false;

	if (pointcloud[fj]->rows() > pointcloud[fi]->rows())
	{
		int temp = fi;
		fi = fj;
		fj = temp;
		swapped = true;
	}

	int nPti = pointcloud[fi]->rows();
	int nPtj = pointcloud[fj]->rows();

	///////////////////////////
	/// BUILD FLANNTREE
	///////////////////////////

	// build FLANNTree - fi
	int rows, dim;
	rows = features[fi]->rows();
	dim = features[fi]->cols();

	std::vector<float> dataset_fi(rows * dim);
	flann::Matrix<float> dataset_mat_fi(&dataset_fi[0], rows, dim);

	for (int i = 0; i < rows; i++)
		for (int j = 0; j < dim; j++)
			dataset_fi[i * dim + j] = static_cast<float>((*features[fi])(i, j));

	flann::Index<flann::L1<float>> feature_tree_i(dataset_mat_fi, flann::KDTreeSingleIndexParams(15));
	//flann::Index<flann::L2<float>> feature_tree_i(dataset_mat_fi, flann::KDTreeIndexParams(4));
	feature_tree_i.buildIndex();

	// build FLANNTree - fj
	rows = features[fj]->rows();
	dim = features[fj]->cols();

	std::vector<float> dataset_fj(rows * dim);
	flann::Matrix<float> dataset_mat_fj(&dataset_fj[0], rows, dim);

	for (int i = 0; i < rows; i++)
		for (int j = 0; j < dim; j++)
			dataset_fj[i * dim + j] = static_cast<float>((*features[fj])(i, j));

	flann::Index<flann::L1<float>> feature_tree_j(dataset_mat_fj, flann::KDTreeSingleIndexParams(15));
	//flann::Index<flann::L2<float>> feature_tree_j(dataset_mat_fj, flann::KDTreeIndexParams(4));
	feature_tree_j.buildIndex();

	bool crosscheck = true;
	bool tuple = true;

	std::vector<int> corres_K, corres_K2;
	std::vector<float> dis;
	std::vector<int> ind;

	//std::vector<std::pair<int, int>> corres;
	std::vector<std::pair<int, int>> corres_cross;
	std::vector<std::pair<int, int>> corres_ij;
	std::vector<std::pair<int, int>> corres_ji;

	///////////////////////////
	/// INITIAL MATCHING
	///////////////////////////

	std::vector<int> i_to_j(nPti, -1);
	for (int j = 0; j < nPtj; j++)
	{
		Eigen::VectorXf query_feature_j = features[fj]->row(j).transpose().cast<float>();
		SearchFLANNTree(&feature_tree_i, query_feature_j, corres_K, dis, 1);
		int i = corres_K[0];
		if (i_to_j[i] == -1)
		{
			Eigen::VectorXf query_feature_i = features[fi]->row(i).transpose().cast<float>();
			SearchFLANNTree(&feature_tree_j, query_feature_i, corres_K, dis, 1);
			int ij = corres_K[0];
			i_to_j[i] = ij;
		}
		corres_ji.push_back(std::pair<int, int>(i, j));
	}

	for (int i = 0; i < nPti; i++)
	{
		if (i_to_j[i] != -1)
			corres_ij.push_back(std::pair<int, int>(i, i_to_j[i]));
	}

	int ncorres_ij = corres_ij.size();
	int ncorres_ji = corres_ji.size();

	// corres = corres_ij + corres_ji;
	for (int i = 0; i < ncorres_ij; ++i)
		corres.push_back(std::pair<int, int>(corres_ij[i].first, corres_ij[i].second));
	for (int j = 0; j < ncorres_ji; ++j)
		corres.push_back(std::pair<int, int>(corres_ji[j].first, corres_ji[j].second));

	printf("points are remained : %d\n", (int)corres.size());

	///////////////////////////
	/// CROSS CHECK
	/// input : corres_ij, corres_ji
	/// output : corres
	///////////////////////////
	if (crosscheck)
	{
		printf("\t[cross check] ");

		// build data structure for cross check
		corres.clear();
		corres_cross.clear();
		std::vector<std::vector<int>> Mi(nPti);
		std::vector<std::vector<int>> Mj(nPtj);

		int ci, cj;
		for (int i = 0; i < ncorres_ij; ++i)
		{
			ci = corres_ij[i].first;
			cj = corres_ij[i].second;
			Mi[ci].push_back(cj);
		}
		for (int j = 0; j < ncorres_ji; ++j)
		{
			ci = corres_ji[j].first;
			cj = corres_ji[j].second;
			Mj[cj].push_back(ci);
		}

		// cross check
		for (int i = 0; i < nPti; ++i)
		{
			for (int ii = 0; ii < Mi[i].size(); ++ii)
			{
				int j = Mi[i][ii];
				for (int jj = 0; jj < Mj[j].size(); ++jj)
				{
					if (Mj[j][jj] == i)
					{
						corres.push_back(std::pair<int, int>(i, j));
						corres_cross.push_back(std::pair<int, int>(i, j));
					}
				}
			}
		}
		printf("points are remained : %d\n", (int)corres.size());
	}

	///////////////////////////
	/// TUPLE CONSTRAINT
	/// input : corres
	/// output : corres
	///////////////////////////
	if (tuple)
	{
		srand(time(NULL));

		printf("\t[tuple constraint] ");
		int rand0, rand1, rand2;
		int idi0, idi1, idi2;
		int idj0, idj1, idj2;
		float scale = TUPLE_SCALE;
		int ncorr = corres.size();
		int number_of_trial = ncorr * 1000;
		std::vector<std::pair<int, int>> corres_tuple;
		std::vector<double> ratios;
		ratios.reserve(TUPLE_MAX_CNT);
		int cnt = 0;
		double all_scale = 0;
		int i;
		for (i = 0; i < number_of_trial; i++)
		{
			rand0 = rand() % ncorr;
			rand1 = rand() % ncorr;
			rand2 = rand() % ncorr;

			idi0 = corres[rand0].first;
			idj0 = corres[rand0].second;
			idi1 = corres[rand1].first;
			idj1 = corres[rand1].second;
			idi2 = corres[rand2].first;
			idj2 = corres[rand2].second;

			if (idi0 == idi1 || idi0 == idi2 || idi1 == idi2)
			{
				continue;
			}
			// collect 3 points from i-th fragment
			Eigen::Vector3f pti0 = pointcloud[fi]->row(idi0).transpose().cast<float>();
			Eigen::Vector3f pti1 = pointcloud[fi]->row(idi1).transpose().cast<float>();
			Eigen::Vector3f pti2 = pointcloud[fi]->row(idi2).transpose().cast<float>();

			float li0 = (pti0 - pti1).norm();
			float li1 = (pti1 - pti2).norm();
			float li2 = (pti2 - pti0).norm();

			// collect 3 points from j-th fragment
			Eigen::Vector3f ptj0 = pointcloud[fj]->row(idj0).transpose().cast<float>();
			Eigen::Vector3f ptj1 = pointcloud[fj]->row(idj1).transpose().cast<float>();
			Eigen::Vector3f ptj2 = pointcloud[fj]->row(idj2).transpose().cast<float>();

			float lj0 = (ptj0 - ptj1).norm();
			float lj1 = (ptj1 - ptj2).norm();
			float lj2 = (ptj2 - ptj0).norm();
#ifndef TUPLE_SIMILAR_CRITERIA
			if ((li0 * scale < lj0) && (lj0 < li0 / scale) &&
				(li1 * scale < lj1) && (lj1 < li1 / scale) &&
				(li2 * scale < lj2) && (lj2 < li2 / scale))
			{
				corres_tuple.push_back(std::pair<int, int>(idi0, idj0));
				corres_tuple.push_back(std::pair<int, int>(idi1, idj1));
				corres_tuple.push_back(std::pair<int, int>(idi2, idj2));
				cnt++;
			}
#else
			float k0 = li0 / lj0;
			float k1 = li1 / lj1;
			float k2 = li2 / lj2;
			if ((k0 * k0 / (k1 * k2) > scale) && (k1 * k2 / (k0 * k0) > scale) &&
				(k1 * k1 / (k0 * k2) > scale) && (k0 * k2 / (k1 * k1) > scale) &&
				(k2 * k2 / (k1 * k0) > scale) && (k1 * k0 / (k2 * k2) > scale) /*&& (k0>1/3) && (k0<3)*/)
			{
				corres_tuple.emplace_back(idi0, idj0);
				corres_tuple.emplace_back(idi1, idj1);
				corres_tuple.emplace_back(idi2, idj2);
				ratios.emplace_back(std::log(std::pow(k0 * k1 * k2, 1.0 / 3)));
				printf("Coarse scale: %f\n", k0);
				all_scale += k0;
				cnt++;
			}
#endif
			if (cnt >= TUPLE_MAX_CNT)
				break;
		}

		printf("all scale: %f\n", all_scale/cnt);
		global_all_scale = all_scale / cnt;

		printf("%d tuples (%d trial, %d actual).\n", cnt, number_of_trial, i);

		corres.clear();
#ifdef STD_FILTER
		//Eigen::Map<Eigen::VectorXd> ratio_vec(&ratios[0], ratios.size());
		//igl::writeDMAT("ratios.dmat", ratio_vec);

		double sum = std::accumulate(ratios.begin(), ratios.end(), 0.0);
		double mean = sum / ratios.size();

		std::vector<double> diff(ratios.size());
		std::transform(ratios.begin(), ratios.end(), diff.begin(), [mean](double x) { return x - mean; });
		double sq_sum = std::inner_product(diff.begin(), diff.end(), diff.begin(), 0.0);
		double stdev = std::sqrt(sq_sum / ratios.size());
		std::cout << "mean: " << mean << "  ; std: " << stdev << std::endl;

		//auto tmp_ratios = ratios;
		//std::nth_element(tmp_ratios.begin(), tmp_ratios.begin() + tmp_ratios.size() / 2, tmp_ratios.end());
		//const double median = tmp_ratios[tmp_ratios.size() / 2];
		//std::cout << "median: " << median << std::endl;
		std::vector<int> feature_mark_i(pointcloud[fi]->rows(), 0);
		std::vector<int> feature_mark_j(pointcloud[fj]->rows(), 0);
		std::set<std::pair<int, int> > corres_set;
		for (int i = 0; i < ratios.size(); ++i)
		{
			if (ratios[i] > mean - stdev && ratios[i] < mean + stdev)
			{
				//corres.emplace_back(corres_tuple[i * 3].first, corres_tuple[i * 3].second);
				//corres.emplace_back(corres_tuple[i * 3 + 1].first, corres_tuple[i * 3 + 1].second);
				//corres.emplace_back(corres_tuple[i * 3 + 2].first, corres_tuple[i * 3 + 2].second);

				feature_mark_i[corres_tuple[i * 3].first]++;
				feature_mark_i[corres_tuple[i * 3 + 1].first]++;
				feature_mark_i[corres_tuple[i * 3 + 2].first]++;

				feature_mark_j[corres_tuple[i * 3].second]++;
				feature_mark_j[corres_tuple[i * 3 + 1].second]++;
				feature_mark_j[corres_tuple[i * 3 + 2].second]++;
				for (int j = 0; j < 3; ++j)
				{
					if (feature_mark_i[corres_tuple[i * 3 + j].first] > 2 && feature_mark_j[corres_tuple[i * 3 + j].second] > 2)
					{
						corres_set.emplace(corres_tuple[i * 3 + j].first, corres_tuple[i * 3 + j].second);
						//corres.emplace_back(corres_tuple[i * 3 + j].first, corres_tuple[i * 3 + j].second);
					}
				}
			}
		}
		for (const auto& ele : corres_set)
		{
			corres.emplace_back(ele);
		}
#else
		for(int itr = 0; itr < corres_tuple.size(); ++itr)
		{
			corres.emplace_back(corres_tuple[itr].first, corres_tuple[itr].second);
		}
#endif


		//std::vector<std::pair<int, int> > trusted_corres;
		////again we random tuples
		//std::random_device rd;
		//std::uniform_int_distribution<> dist_trust(0, trusted_corres.size() - 1);
		//std::uniform_int_distribution<> dist_all(0, corres.size() - 1);
		//corres_tuple.clear();
		//ratios.clear();
		//cnt = 0;
		//for (i = 0; i < number_of_trial; i++)
		//{
		//	rand0 = dist_trust(rd);
		//	rand1 = dist_trust(rd);
		//	rand2 = dist_all(rd);

		//	idi0 = trusted_corres[rand0].first;
		//	idj0 = trusted_corres[rand0].second;
		//	idi1 = trusted_corres[rand1].first;
		//	idj1 = trusted_corres[rand1].second;
		//	idi2 = corres[rand2].first;
		//	idj2 = corres[rand2].second;

		//	if (idi0 == idi1 || idi0 == idi2 || idi1 == idi2)
		//	{
		//		continue;
		//	}

		//	// collect 3 points from i-th fragment
		//	Eigen::Vector3d pti0 = pointcloud[fi]->row(idi0).transpose();
		//	Eigen::Vector3d pti1 = pointcloud[fi]->row(idi1).transpose();
		//	Eigen::Vector3d pti2 = pointcloud[fi]->row(idi2).transpose();

		//	double li0 = (pti0 - pti1).norm();
		//	double li1 = (pti1 - pti2).norm();
		//	double li2 = (pti2 - pti0).norm();

		//	// collect 3 points from j-th fragment
		//	Eigen::Vector3d ptj0 = pointcloud[fj]->row(idj0).transpose();
		//	Eigen::Vector3d ptj1 = pointcloud[fj]->row(idj1).transpose();
		//	Eigen::Vector3d ptj2 = pointcloud[fj]->row(idj2).transpose();

		//	double lj0 = (ptj0 - ptj1).norm();
		//	double lj1 = (ptj1 - ptj2).norm();
		//	double lj2 = (ptj2 - ptj0).norm();

		//	double k0 = li0 / lj0;
		//	double k1 = li1 / lj1;
		//	double k2 = li2 / lj2;
		//	if ((k0 * k0 / (k1 * k2) > scale) && (k1 * k2 / (k0 * k0) > scale) &&
		//		(k1 * k1 / (k0 * k2) > scale) && (k0 * k2 / (k1 * k1) > scale) &&
		//		(k2 * k2 / (k1 * k0) > scale) && (k1 * k0 / (k2 * k2) > scale))
		//	{
		//		corres_tuple.emplace_back(idi0, idj0);
		//		corres_tuple.emplace_back(idi1, idj1);
		//		corres_tuple.emplace_back(idi2, idj2);
		//		ratios.emplace_back(std::log(std::pow(k0 * k1 * k2, 1.0 / 3)));
		//		cnt++;
		//	}
		//	if (cnt >= TUPLE_MAX_CNT)
		//		break;
		//}
		//printf("%d tuples (%d trial, %d actual).\n", cnt, number_of_trial, i);

		//sum = std::accumulate(ratios.begin(), ratios.end(), 0.0);
		//mean = sum / ratios.size();

		//diff.resize(ratios.size());
		//std::transform(ratios.begin(), ratios.end(), diff.begin(), [mean](double x) { return x - mean; });
		//sq_sum = std::inner_product(diff.begin(), diff.end(), diff.begin(), 0.0);
		//stdev = std::sqrt(sq_sum / ratios.size());
		//std::cout << "mean: " << mean << "  ; std: " << stdev << std::endl;

		//feature_mark_i.resize(pointcloud[fi]->rows(), 0);
		//feature_mark_j.resize(pointcloud[fj]->rows(), 0);
		//corres.clear();
		//for (int i = 0; i < ratios.size(); ++i)
		//{
		//	if (ratios[i] > mean - stdev && ratios[i] < mean + stdev)
		//	{
		//		corres_set.emplace(corres_tuple[i * 3].first, corres_tuple[i * 3].second);
		//		corres_set.emplace(corres_tuple[i * 3 + 1].first, corres_tuple[i * 3 + 1].second);
		//		corres_set.emplace(corres_tuple[i * 3 + 2].first, corres_tuple[i * 3 + 2].second);
		//		//corres.emplace_back(corres_tuple[i * 3].first, corres_tuple[i * 3].second);
		//		//corres.emplace_back(corres_tuple[i * 3 + 1].first, corres_tuple[i * 3 + 1].second);
		//		//corres.emplace_back(corres_tuple[i * 3 + 2].first, corres_tuple[i * 3 + 2].second);

		//		//feature_mark_i[corres_tuple[i * 3].first]++;
		//		//feature_mark_i[corres_tuple[i * 3 + 1].first]++;
		//		//feature_mark_i[corres_tuple[i * 3 + 2].first]++;

		//		//feature_mark_j[corres_tuple[i * 3].second]++;
		//		//feature_mark_j[corres_tuple[i * 3 + 1].second]++;
		//		//feature_mark_j[corres_tuple[i * 3 + 2].second]++;
		//		//for (int j = 0; j < 3; ++j)
		//		//{
		//		//	if (feature_mark_i[corres_tuple[i * 3 + j].first] > 2 && feature_mark_j[corres_tuple[i * 3 + j].second] > 2)
		//		//	{
		//		//		corres_set.emplace(corres_tuple[i * 3 + j].first, corres_tuple[i * 3 + j].second);
		//		//		//corres.emplace_back(corres_tuple[i * 3 + j].first, corres_tuple[i * 3 + j].second);
		//		//	}
		//		//}
		//	}
		//}
		//for (const auto& ele : corres_set)
		//{
		//	corres.emplace_back(ele);
		//}
	}

	if (swapped)
	{
		std::vector<std::pair<int, int>> temp;
		for (int i = 0; i < corres.size(); i++)
			temp.push_back(std::pair<int, int>(corres[i].second, corres[i].first));
		corres.clear();
		corres = temp;
	}

	printf("\t[final] matches %d.\n", (int)corres.size());
}

double FastGlobalRegistration::optimize_pairwise(bool decrease_mu, int num_iter, const Eigen::MatrixXd& v_1, const Eigen::MatrixXd& v_2, const std::vector<std::pair<int, int> >& corres, Eigen::Matrix4d& trans_mat)
{
	printf("Pairwise rigid pose optimization\n");
	if (corres.size() < 10) return -1;

	double par = 4.0f;

	// make a float copy of v2.
	//global_all_scale = 1;

	//Eigen::MatrixXd pcj_copy = v_2 * global_all_scale;
	Eigen::MatrixXd pcj_copy = v_2;

	Eigen::Matrix4d trans;
	trans.setIdentity();

	for (int itr = 0; itr < num_iter; itr++)
	{
		//global_iter = itr;
		//printf("global_iter %d\n", global_iter);
		// graduated non-convexity.
		if (decrease_mu)
		{
			if (itr % 4 == 0 && par > MAX_CORR_DIST) {
				par /= DIV_FACTOR;
			}
		}

		Eigen::Matrix4d delta = update_ssicp(v_1, pcj_copy, corres, par);
		//Eigen::Matrix4f delta = update_fgr(v_1, pcj_copy, corres, par);

		trans = delta * trans.eval();

		// transform point clouds
		pcj_copy = (pcj_copy.rowwise().homogeneous() * delta.transpose()).leftCols(3).eval();
	}
	//trans_mat = trans;
	trans_mat = trans;
	trans_mat(3, 3) = 1;
	return par;
}


Eigen::Matrix4f FastGlobalRegistration::update_fgr(const Eigen::MatrixXd &v_1, const Eigen::MatrixXf &v_2,
	const std::vector<std::pair<int, int>> &corres, const double mu)
{
	const int nvariable = 6;	// 3 for rotation and 3 for translation
	Eigen::MatrixXd JTJ(nvariable, nvariable);
	Eigen::MatrixXd JTr(nvariable, 1);
	Eigen::MatrixXd J(nvariable, 1);
	JTJ.setZero();
	JTr.setZero();

	std::vector<double> s(corres.size(), 1.0);
	double r;
	double r2 = 0.0;
	double e = 0;
	for (int c = 0; c < corres.size(); c++) {
		int ii = corres[c].first;
		int jj = corres[c].second;
		Eigen::Vector3f p, q;
		p = v_1.row(ii).transpose().cast<float>();
		q = v_2.row(jj).transpose();
		Eigen::Vector3f rpq = p - q;

		int c2 = c;

		float temp = mu / (rpq.dot(rpq) + mu);
		s[c2] = temp * temp;

		e += temp * temp*rpq.dot(rpq) + mu * (temp - 1)*(temp - 1);

		J.setZero();
		J(1) = -q(2);
		J(2) = q(1);
		J(3) = -1;
		r = rpq(0);
		JTJ += J * J.transpose() * s[c2];
		JTr += J * r * s[c2];
		r2 += r * r * s[c2];

		J.setZero();
		J(2) = -q(0);
		J(0) = q(2);
		J(4) = -1;
		r = rpq(1);
		JTJ += J * J.transpose() * s[c2];
		JTr += J * r * s[c2];
		r2 += r * r * s[c2];

		J.setZero();
		J(0) = -q(1);
		J(1) = q(0);
		J(5) = -1;
		r = rpq(2);
		JTJ += J * J.transpose() * s[c2];
		JTr += J * r * s[c2];
		r2 += r * r * s[c2];

		r2 += (mu * (1.0 - sqrt(s[c2])) * (1.0 - sqrt(s[c2])));
	}
	std::cout << "energy: " << e << std::endl;

	Eigen::MatrixXd result(nvariable, 1);
	result = -JTJ.llt().solve(JTr);

	Eigen::Affine3d aff_mat;
	aff_mat.linear() = (Eigen::Matrix3d) Eigen::AngleAxisd(result(2), Eigen::Vector3d::UnitZ())
		* Eigen::AngleAxisd(result(1), Eigen::Vector3d::UnitY())
		* Eigen::AngleAxisd(result(0), Eigen::Vector3d::UnitX());
	aff_mat.translation() = Eigen::Vector3d(result(3), result(4), result(5));

	return aff_mat.matrix().cast<float>();
}

Eigen::Matrix3d FastGlobalRegistration::compute_r(const Eigen::MatrixXd &p_mat, const Eigen::MatrixXd &q_mat, const double mu)
{
	const int nvariable = 3;	// 3 for rotation
	Eigen::MatrixXd JTJ(nvariable, nvariable);
	Eigen::MatrixXd JTr(nvariable, 1);
	Eigen::MatrixXd J(nvariable, 1);
	JTJ.setZero();
	JTr.setZero();

	std::vector<double> s(p_mat.rows(), 1.0);
	double r;
	double r2 = 0.0;
	for (int c = 0; c < s.size(); c++) {
		Eigen::Vector3d p = p_mat.row(c).transpose();
		Eigen::Vector3d q = q_mat.row(c).transpose();
		Eigen::Vector3d rpq = p - q;

		int c2 = c;

		double temp = mu / (rpq.dot(rpq) + mu);
		s[c2] = temp * temp;

		J.setZero();
		J(1) = -q(2);
		J(2) = q(1);
		r = rpq(0);
		JTJ += J * J.transpose() * s[c2];
		JTr += J * r * s[c2];
		r2 += r * r * s[c2];

		J.setZero();
		J(2) = -q(0);
		J(0) = q(2);
		r = rpq(1);
		JTJ += J * J.transpose() * s[c2];
		JTr += J * r * s[c2];
		r2 += r * r * s[c2];

		J.setZero();
		J(0) = -q(1);
		J(1) = q(0);
		r = rpq(2);
		JTJ += J * J.transpose() * s[c2];
		JTr += J * r * s[c2];
		r2 += r * r * s[c2];

		r2 += (mu * (1.0 - sqrt(s[c2])) * (1.0 - sqrt(s[c2])));
	}

	Eigen::MatrixXd result(nvariable, 1);
	result = -JTJ.llt().solve(JTr);
	Eigen::Matrix3d r_mat;
	r_mat = Eigen::AngleAxisd(result(2), Eigen::Vector3d::UnitZ())
		* Eigen::AngleAxisd(result(1), Eigen::Vector3d::UnitY())
		* Eigen::AngleAxisd(result(0), Eigen::Vector3d::UnitX());
	return r_mat;
}

Eigen::Matrix4d FastGlobalRegistration::update_ssicp(const Eigen::MatrixXd &v_1, const Eigen::MatrixXd &v_2,
	const std::vector<std::pair<int, int> >& corres, const double mu)
{
#ifdef SHOW_DEBUG_INFO
	double e = 0;
#endif
	double sum_l = 0;
	Eigen::RowVector3d sum_l_p(0, 0, 0);
	Eigen::RowVector3d sum_l_q(0, 0, 0);
	Eigen::VectorXd sqrtl_vec(corres.size());
	Eigen::MatrixXd X(corres.size(), 3), Z(corres.size(), 3);
	for (size_t i = 0; i < corres.size(); ++i)
	{
		Eigen::Vector3d p = v_1.row(corres[i].first).transpose();
		Eigen::Vector3d q = v_2.row(corres[i].second).transpose();
		Eigen::Vector3d rpq = p - q;
		double sqrtl = mu / (rpq.dot(rpq) + mu);
		sqrtl_vec(i) = sqrtl;
		double l = sqrtl * sqrtl;
		sum_l += l;
		sum_l_p += l * p.transpose().cast<double>();
		sum_l_q += l * q.transpose().cast<double>();
#ifdef SHOW_DEBUG_INFO
		e += sqrtl * sqrtl*rpq.dot(rpq) + mu * (sqrtl - 1)*(sqrtl - 1);
#endif

		X.row(i) = sqrtl * v_2.row(corres[i].second);
		Z.row(i) = sqrtl * v_1.row(corres[i].first);
	}
	X -= (sqrtl_vec * sum_l_q / sum_l);
	Z -= (sqrtl_vec * sum_l_p / sum_l);

#ifdef SHOW_DEBUG_INFO
	std::cout << "energy: " << e << std::endl;
	std:fstream file("D:\\reg\\reg.txt", ios::app);
	file << "energy: " << e << std::endl;
#endif
#ifdef USE_RANSAC
	Eigen::MatrixXi corres_mat(corres.size(), 2);
	for (int i = 0; i < corres.size(); ++i)
	{
		corres_mat(i, 0) = corres[i].first;
		corres_mat(i, 1) = corres[i].second;
	}
	Eigen::MatrixXd feature_1 = igl::slice(v_1, corres_mat.col(0), 1);
	Eigen::MatrixXf feature_2 = igl::slice(v_2, corres_mat.col(1), 1);

	pcl::PointCloud<pcl::PointXYZ>::Ptr pointcloud_1(new pcl::PointCloud<pcl::PointXYZ>);
	pcl::PointCloud<pcl::PointXYZ>::Ptr pointcloud_2(new pcl::PointCloud<pcl::PointXYZ>);
	pointcloud_1->points.resize(feature_1.rows());
	pointcloud_2->points.resize(feature_2.rows());
	for (int i = 0; i < corres_mat.rows(); ++i)
	{
		for (int j = 0; j < 3; ++j)
		{
			pointcloud_1->points[i].data[j] = static_cast<float>(feature_1(i, j));
			pointcloud_2->points[i].data[j] = static_cast<float>(feature_2(i, j));
		}
		pointcloud_1->points[i].data[3] = 1.0f;
		pointcloud_2->points[i].data[3] = 1.0f;
	}

	pcl::SampleConsensusModelRegistration<pcl::PointXYZ>::Ptr model_r(new pcl::SampleConsensusModelRegistration<pcl::PointXYZ>(pointcloud_2));
	model_r->setInputTarget(pointcloud_1);
	Eigen::VectorXf coeff;
	pcl::RandomSampleConsensus<pcl::PointXYZ> ransac(model_r, 0.05);
	ransac.computeModel(0);
	ransac.getModelCoefficients(coeff);
	Eigen::Matrix4f transform_ransac;
	std::cout << "RANSAC transformation: " << std::endl;
	for (size_t i = 0; i < 16; i++) {
		transform_ransac(i / 4, i % 4) = coeff[i];
	}
	std::cout << transform_ransac << std::endl;
	std::vector<int> inlier_vec;
	ransac.getInliers(inlier_vec);
	Eigen::VectorXi inlier_ids = Eigen::Map<Eigen::VectorXi>(&inlier_vec[0], inlier_vec.size());
	std::cout << "inlier num: " << inlier_ids.rows() << std::endl;
	Z = igl::slice(feature_1, inlier_ids, 1);
	X = igl::slice(feature_2, inlier_ids, 1).cast<double>();
#endif

	Eigen::Matrix3d R = FastGlobalRegistration::compute_r(Z, X, mu);

	double num = (Z.array() * (X * R.transpose()).array()).sum();
	double den = X.squaredNorm();
	double s = num / den;

	printf("estimate scale: %f",s);

	if (s < 0)
	{
		s = 1;
	}

	Eigen::RowVector3d T = sum_l_p / sum_l - s * sum_l_q / sum_l * (R.transpose());

	Eigen::Matrix4d trans;
	trans.setZero();
	trans.block<3, 3>(0, 0) = s * R;
	trans.block<3, 1>(0, 3) = T.transpose();
	trans(3, 3) = 1;

	return trans;
}

double FastGlobalRegistration::optimize_global(bool decrease_mu, int num_iter, std::map<int, Eigen::MatrixXd>& v_map, std::map<int, std::map<int, std::vector<std::pair<int, int> > > >& corres_map, std::map<int, Eigen::Matrix4d>& trans_mat_map)
{
	printf("Global pose optimization\n");

	double par = 4.0f;

	std::map<int, Eigen::MatrixXd> backup_v_map = v_map;
	std::map<int, double> scales;
	for (const auto& ele : v_map)
	{
		trans_mat_map[ele.first] = Eigen::Matrix4d::Identity();
	}

	for (int itr = 0; itr < num_iter; itr++)
	{
		// graduated non-convexity.
		if (decrease_mu)
		{
			if (itr % 4 == 0 && par > MAX_CORR_DIST) {
				par /= DIV_FACTOR;
			}
		}

		if (!trans_mat_map.empty())
		{
			auto temp_trans_map = update_ssicp_global(backup_v_map, corres_map, par);
			for (auto& ele : trans_mat_map)
			{
				ele.second = temp_trans_map[ele.first] * ele.second.eval();
				backup_v_map[ele.first] = (v_map[ele.first].eval().rowwise().homogeneous() * ele.second.transpose()).leftCols(3);
			}
		}
		else
		{
			trans_mat_map = update_ssicp_global(backup_v_map, corres_map, par);
		}

		//if (itr < 50)
		//{
		//	continue;
		//}

		int func_num = 0;
		for (const auto& outer_ele : corres_map)
		{
			for (const auto& inner_ele : outer_ele.second)
			{
				func_num += inner_ele.second.size();
			}
		}
		Eigen::SparseMatrix<double> jacobian_mat(func_num * 3 + 1, v_map.size());
		Eigen::VectorXd residuals(func_num * 3 + 1);

		std::vector<Eigen::Triplet<double> > triplets;
		triplets.reserve(func_num * 6 + 1);
		int counter = 0;
		for (const auto& outer_ele : corres_map)
		{
			const Eigen::MatrixXd& pointcloud_1 = v_map.find(outer_ele.first)->second;
			Eigen::MatrixXd& trans_pointcloud_1 = backup_v_map.find(outer_ele.first)->second;
			Eigen::Matrix3d rotation_1 = trans_mat_map[outer_ele.first].block<3, 3>(0, 0) / scales[outer_ele.first];
			Eigen::MatrixXd rotated_pc1 = pointcloud_1 * rotation_1.transpose();
			for (const auto& inner_ele : outer_ele.second)
			{
				const Eigen::MatrixXd& pointcloud_2 = v_map.find(inner_ele.first)->second;
				Eigen::MatrixXd& trans_pointcloud_2 = backup_v_map.find(inner_ele.first)->second;

				Eigen::Matrix3d rotation_2 = trans_mat_map[inner_ele.first].block<3, 3>(0, 0) / scales[inner_ele.first];
				Eigen::MatrixXd rotated_pc2 = pointcloud_2 * rotation_2.transpose();
				for (const auto& pair_ele : inner_ele.second)
				{
					Eigen::RowVector3d p = trans_pointcloud_1.row(pair_ele.first);
					Eigen::RowVector3d q = trans_pointcloud_2.row(pair_ele.second);
					Eigen::RowVector3d rpq = p - q;
					const double sqrtl = par / (rpq.dot(rpq) + par);
					p *= sqrtl;
					q *= sqrtl;

					residuals(counter) = p(0) - q(0);
					residuals(counter + 1) = p(1) - q(1);
					residuals(counter + 2) = p(2) - q(2);

					triplets.emplace_back(counter, outer_ele.first, sqrtl * rotated_pc1(pair_ele.first, 0));
					triplets.emplace_back(counter, inner_ele.first, -sqrtl * rotated_pc2(pair_ele.second, 0));
					++counter;

					triplets.emplace_back(counter, outer_ele.first, sqrtl * rotated_pc1(pair_ele.first, 1));
					triplets.emplace_back(counter, inner_ele.first, -sqrtl * rotated_pc2(pair_ele.second, 1));
					++counter;

					triplets.emplace_back(counter, outer_ele.first, sqrtl * rotated_pc1(pair_ele.first, 2));
					triplets.emplace_back(counter, inner_ele.first, -sqrtl * rotated_pc2(pair_ele.second, 2));
					++counter;
				}
			}
		}
		triplets.emplace_back(counter, 0, 1e6);
		residuals(counter) = 0;

		jacobian_mat.setFromTriplets(triplets.begin(), triplets.end());
		const Eigen::SparseMatrix<double> jacobian_mat_trans = jacobian_mat.transpose();
		Eigen::SparseMatrix<double> jtj = jacobian_mat_trans * jacobian_mat;
		const Eigen::VectorXd jtr = jacobian_mat_trans * residuals;
		Eigen::SimplicialLDLT<Eigen::SparseMatrix<double> > solver(jtj);
		Eigen::VectorXd result = -solver.solve(jtr);

		for (auto& ele : trans_mat_map)
		{
			ele.second.block<3, 3>(0, 0) *= 1 + result(ele.first) / scales[ele.first];
			scales[ele.first] += result(ele.first);
			backup_v_map[ele.first] = (v_map[ele.first].eval().rowwise().homogeneous() * ele.second.transpose()).leftCols(3);
		}
	}
	return par;
}

std::map<int, Eigen::Matrix4d> FastGlobalRegistration::update_ssicp_global(std::map<int, Eigen::MatrixXd>& v_map, std::map<int, std::map<int, std::vector<std::pair<int, int> > > >& corres_map, const double mu)
{
	int func_num = 0;
	for (const auto& outer_ele : corres_map)
	{
		for (const auto& inner_ele : outer_ele.second)
		{
			func_num += inner_ele.second.size();
		}
	}

	std::map<int, Eigen::Matrix4d> trans_map;
	for (const auto& ele : v_map)
	{
		trans_map[ele.first] = Eigen::Matrix4d::Identity();
	}
	//Eigen::MatrixXd jacobian_mat(func_num * 3 + 6, 6 * v_map.size());
	std::vector<Eigen::Triplet<double> > triplets;
	triplets.reserve(func_num * 3 * 6 + 6);

	Eigen::SparseMatrix<double> jacobian_mat(func_num * 3 + 6, 6 * v_map.size());
	Eigen::VectorXd residuals(func_num * 3 + 6);
	int counter = 0;
	for (const auto& outer_ele : corres_map)
	{
		const Eigen::MatrixXd& pointcloud_1 = v_map.find(outer_ele.first)->second;
		for (const auto& inner_ele : outer_ele.second)
		{
			const Eigen::MatrixXd& pointcloud_2 = v_map.find(inner_ele.first)->second;
			for (const auto& pair_ele : inner_ele.second)
			{
				Eigen::RowVector3d p = pointcloud_1.row(pair_ele.first);
				Eigen::RowVector3d q = pointcloud_2.row(pair_ele.second);

				Eigen::RowVector3d rpq = p - q;
				const double sqrtl = mu / (rpq.dot(rpq) + mu);

				p *= sqrtl;
				q *= sqrtl;

				residuals(counter) = p(0) - q(0);
				residuals(counter + 1) = p(1) - q(1);
				residuals(counter + 2) = p(2) - q(2);
				//x
				triplets.emplace_back(counter, outer_ele.first * 6 + 1, p(2));
				triplets.emplace_back(counter, outer_ele.first * 6 + 2, -p(1));
				triplets.emplace_back(counter, outer_ele.first * 6 + 3, sqrtl);
				triplets.emplace_back(counter, inner_ele.first * 6 + 1, -q(2));
				triplets.emplace_back(counter, inner_ele.first * 6 + 2, q(1));
				triplets.emplace_back(counter, inner_ele.first * 6 + 3, -sqrtl);
				++counter;

				//y
				triplets.emplace_back(counter, outer_ele.first * 6, -p(2));
				triplets.emplace_back(counter, outer_ele.first * 6 + 2, p(0));
				triplets.emplace_back(counter, outer_ele.first * 6 + 4, sqrtl);
				triplets.emplace_back(counter, inner_ele.first * 6, q(2));
				triplets.emplace_back(counter, inner_ele.first * 6 + 2, -q(0));
				triplets.emplace_back(counter, inner_ele.first * 6 + 4, -sqrtl);
				++counter;

				//z
				triplets.emplace_back(counter, outer_ele.first * 6, p(1));
				triplets.emplace_back(counter, outer_ele.first * 6 + 1, -p(0));
				triplets.emplace_back(counter, outer_ele.first * 6 + 5, sqrtl);
				triplets.emplace_back(counter, inner_ele.first * 6, -q(1));
				triplets.emplace_back(counter, inner_ele.first * 6 + 1, q(0));
				triplets.emplace_back(counter, inner_ele.first * 6 + 5, -sqrtl);
				++counter;
			}

		}
	}
	triplets.emplace_back(counter, 0, 1e5);
	triplets.emplace_back(counter + 1, 1, 1e5);
	triplets.emplace_back(counter + 2, 2, 1e5);
	triplets.emplace_back(counter + 3, 3, 1e5);
	triplets.emplace_back(counter + 4, 4, 1e5);
	triplets.emplace_back(counter + 5, 5, 1e5);

	residuals(counter) = 0;
	residuals(counter + 1) = 0;
	residuals(counter + 2) = 0;
	residuals(counter + 3) = 0;
	residuals(counter + 4) = 0;
	residuals(counter + 5) = 0;

	jacobian_mat.setFromTriplets(triplets.begin(), triplets.end());
	const Eigen::SparseMatrix<double> jacobian_mat_trans = jacobian_mat.transpose();
	Eigen::SparseMatrix<double> jtj = jacobian_mat_trans * jacobian_mat;
	const Eigen::VectorXd jtr = jacobian_mat_trans * residuals;

	Eigen::SimplicialLDLT<Eigen::SparseMatrix<double> > solver(jtj);
	Eigen::VectorXd result = -solver.solve(jtr);

	for (int i = 0; i < result.rows(); ++i)
	{
		Eigen::Matrix4d& trans_mat = trans_map[i];
		trans_mat.block<3, 3>(0, 0) = static_cast<Eigen::Matrix3d>(Eigen::AngleAxisd(result(i * 6 + 2), Eigen::Vector3d::UnitZ())
			* Eigen::AngleAxisd(result(i * 6 + 1), Eigen::Vector3d::UnitY())
			* Eigen::AngleAxisd(result(i * 6), Eigen::Vector3d::UnitX()));
		trans_mat(0, 3) = result(i * 6 + 3);
		trans_mat(1, 3) = result(i * 6 + 4);
		trans_mat(2, 3) = result(i * 6 + 5);
	}

	return trans_map;
}