/*
	This file is part of the MinSG library extension
	AdaptiveGlobalVisibilitySampling.
	Copyright (C) 2013 Benjamin Eikel <benjamin@eikel.org>
	
	This library is subject to the terms of the Mozilla Public License, v. 2.0.
	You should have received a copy of the MPL along with this library; see the 
	file LICENSE. If not, you can obtain one at http://mozilla.org/MPL/2.0/.
*/
#ifdef MINSG_EXT_ADAPTIVEGLOBALVISIBILITYSAMPLING

#include "SampleDistributions.h"
#include "Definitions.h"
#include "MutationCandidate.h"
#include "MutationCandidates.h"
#include "Sample.h"
#include "../RayCasting/RayCaster.h"
#include "../TriangleTrees/TriangleAccessor.h"
#include "../ValuatedRegion/ValuatedRegionNode.h"
#include "../../Core/Nodes/GeometryNode.h"
#include "../../Core/Transformations.h"
#include "../../Helper/StdNodeVisitors.h"
#include <Geometry/Box.h>
#include <Geometry/Line.h>
#include <Geometry/Sphere.h>
#include <Geometry/Triangle.h>
#include <Geometry/Vec3.h>
#include <Geometry/VecHelper.h>
#include <Rendering/Mesh/Mesh.h>
#include <Rendering/MeshUtils/LocalMeshDataHolder.h>
#include <Util/Timer.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <random>
#include <vector>

#ifndef M_PI
#define M_PI		3.14159265358979323846
#endif

namespace MinSG {
namespace AGVS {

/**
 * Return the singleton instance of a random number generator shared by all
 * distributions.
 */
static std::mt19937 & getGenerator() {
	static std::mt19937 generator(42);
	return generator;
}

template<typename value_t>
struct SampleDistributions::Implementation {
	typedef Geometry::_Vec3<value_t> vec3_t;
	typedef Geometry::_Ray<vec3_t> ray_t;
	typedef Geometry::Triangle<vec3_t> triangle_t;

	std::mt19937 & gen;

	const std::deque<GeometryNode *> objects;

	std::uniform_int_distribution<std::size_t> objectDist;
	std::uniform_real_distribution<value_t> barycentricUDist;
	std::uniform_real_distribution<value_t> zeroOneDist;
	std::uniform_real_distribution<value_t> viewSpaceXDist;
	std::uniform_real_distribution<value_t> viewSpaceYDist;
	std::uniform_real_distribution<value_t> viewSpaceZDist;
	std::uniform_real_distribution<value_t> azimuthDist;

	struct SampleDistribution {
		typedef std::function<Sample<value_t> ()> generator_function_t;
		const generator_function_t generator;

		//! Classification of distribution function (true for mutation-based)
		const bool isMutationBased;

		//! Average time for processing a sample from D (called t_s(D))
		value_t averageTime;

		//! Number of samples generated by D (called |S(D)|)
		uint32_t numSamples;

		//! Contribution of the samples generated by D (called C(D))
		uint32_t contribution;

		//! Number of samples with positive contribution
		uint32_t numContributingSamples;

		SampleDistribution(generator_function_t genFun,
						   bool mutationBased) :
			generator(std::move(genFun)),
			isMutationBased(mutationBased),
			averageTime(0.0),
			numSamples(0),
			contribution(0),
			numContributingSamples(0) {
		}

		//! Calculate the average contribution (called c_s(D))
		value_t getAverageContribution() const {
			if(numSamples == 0) {
				return 1.0;
			}
			return contribution / static_cast<value_t>(numSamples);
		}

		//! Calculate the weight (called w(D))
		value_t getWeight() const {
			return getAverageContribution() / averageTime;
		}

		void clear() {
			numSamples = 0;
			contribution = 0;
			numContributingSamples = 0;
		}
	};

	//! Distribution functions (a distribution is called D in the article)
	std::array<SampleDistribution, 5> sampleDists;

	//! Random distribution using distribution probabilities (called p(D_i))
	std::discrete_distribution<std::size_t> sampleSelectDist;

	MutationCandidates mutationCandidates;

	Implementation(const Geometry::_Box<value_t> & bounds,
				  const GroupNode * scene) :
		gen(getGenerator()),
		objects(collectNodes<GeometryNode>(scene)),
		objectDist(0, objects.size() - 1),
		zeroOneDist(static_cast<value_t>(0.0), static_cast<value_t>(1.0)),
		viewSpaceXDist(bounds.getMinX(), bounds.getMaxX()),
		viewSpaceYDist(bounds.getMinY(), bounds.getMaxY()),
		viewSpaceZDist(bounds.getMinZ(), bounds.getMaxZ()),
		azimuthDist(static_cast<value_t>(0.0), static_cast<value_t>(2.0 * M_PI)),
		sampleDists({{
			{std::bind(&Implementation<value_t>::generateViewSpaceDirectionSample, this),
				false},
			{std::bind(&Implementation<value_t>::generateObjectDirectionSample, this),
				false},
			{std::bind(&Implementation<value_t>::generateTwoPointSample, this),
				false},
			{std::bind(&Implementation<value_t>::generateTwoPointMutationSample, this),
				true},
			{std::bind(&Implementation<value_t>::generateSilhouetteMutationSample, this),
				true}
		}}),
		sampleSelectDist(),
		mutationCandidates() {

		calibrationPass();
		updateDistributionProbabilities();
	}

	//! Update the average processing times
	void calibrationPass() {
		// Constant suggested in the original article
		const uint32_t numSamples = 100000;
		for(auto & dist : sampleDists) {
			// Skip mutation-based distributions if there are no candidates
			if(dist.isMutationBased && mutationCandidates.isEmpty()) {
				dist.averageTime = std::numeric_limits<value_t>::max();
				continue;
			}
			Util::Timer timer;
			timer.reset();
			for(uint_fast32_t s = 0; s < numSamples; ++s) {
				dist.generator();
			}
			timer.stop();
			dist.averageTime = timer.getNanoseconds() / 
												static_cast<value_t>(numSamples);
		}
	}

	void updateDistributionProbabilities() {
		std::size_t numNewSamples = 0;
		for(const auto & dist : sampleDists) {
			numNewSamples += dist.numSamples;
		}
		// The article suggests a calibration pass after 100M samples
		const bool calibrate = numNewSamples > 100000000;
		if(calibrate) {
			calibrationPass();
		}
		std::vector<value_t> weights;
		weights.reserve(sampleDists.size());
		for(const auto & dist : sampleDists) {
			weights.push_back(dist.getWeight());
		}
		sampleSelectDist = std::discrete_distribution<std::size_t>(weights.begin(),
																   weights.end());
		// Clean up sample arrays
		if(calibrate) {
			for(auto & dist : sampleDists) {
				dist.clear();
			}
		}
	}

	Sample<value_t> generateSample() {
		const auto d = sampleSelectDist(gen);
		auto sample = sampleDists[d].generator();
		++sampleDists[d].numSamples;
		sample.setDistributionId(static_cast<uint8_t>(d));
		return sample;
	}

	void updateWithSample(const Sample<value_t> & sample,
						  const contribution_t & contribution,
						  ValuatedRegionNode * viewCell) {
		const auto contributionSum = std::get<0>(contribution) + std::get<1>(contribution);
		const auto d = sample.getDistributionId();
		if(contributionSum > 0) {
			sampleDists[d].contribution += static_cast<uint32_t>(contributionSum);
			mutationCandidates.addMutationCandidate(sample,
													contribution,
													viewCell);
		}
		if(std::get<2>(contribution) > 0) {
			++sampleDists[d].numContributingSamples;
		}
	}

	vec3_t generateRandomViewSpacePoint() {
		return vec3_t(viewSpaceXDist(gen),
					  viewSpaceYDist(gen),
					  viewSpaceZDist(gen));
	}

	GeometryNode * getRandomObject() {
		GeometryNode * object = nullptr;
		uint32_t triangleCount = 0;
		// Ignore nodes without mesh and with other primitives than triangles.
		while(object == nullptr || triangleCount == 0) {
			object = objects[objectDist(gen)];
			triangleCount = object->getTriangleCount();
		}
		return object;
	}

	triangle_t getRandomTriangle(GeometryNode * object) {
		Rendering::Mesh * mesh = object->getMesh();
		const uint32_t triangleCount = object->getTriangleCount();
		Rendering::MeshUtils::LocalMeshDataHolder meshHolder(mesh);

		std::uniform_int_distribution<uint32_t> triangleDist(0, triangleCount - 1);

		// Skip degenerate triangles.
		triangle_t triangle(vec3_t(0, 0, 0), vec3_t(0, 0, 0), vec3_t(0, 0, 0));
		do {
			const auto triangleIndex = triangleDist(gen);
			const TriangleTrees::TriangleAccessor triangleAccessor(mesh, triangleIndex);
			triangle = triangleAccessor.getTriangle();
		} while(triangle.isDegenerate());
		return triangle;
	}

	/**
	 * Generate a random point in view space and a random direction in
	 * directional space.
	 * 
	 * @see paragraph "View space-direction distribution"
	 */
	Sample<value_t> generateViewSpaceDirectionSample() {
		const vec3_t origin = generateRandomViewSpacePoint();
		const value_t inclination = static_cast<value_t>(std::acos(1.0 - 2.0 * zeroOneDist(gen)));
		const value_t azimuth = azimuthDist(gen);
		const auto direction = Geometry::_Sphere<value_t>::calcCartesianCoordinateUnitSphere(inclination, azimuth);
		return Sample<value_t>(ray_t(origin, direction));
	}

	/**
	 * Generate a random point on the surface of a randomly chosen object.
	 * The direction is chosen from a hemisphere above the tangent plane of
	 * the point.
	 * 
	 * @see paragraph "Object-direction distribution"
	 */
	Sample<value_t> generateObjectDirectionSample() {
		GeometryNode * object = getRandomObject();
		const auto triangle = getRandomTriangle(object);

		const auto u = zeroOneDist(gen);
		std::uniform_real_distribution<value_t> barycentricVDist(static_cast<value_t>(0.0), static_cast<value_t>(1.0) - u);
		const auto origin = Transformations::localPosToWorldPos(*object, triangle.calcPoint(u, barycentricVDist(gen)));

		Geometry::_Matrix3x3<value_t> rotation;
		rotation.setRotation(triangle.getEdgeAB(), triangle.calcNormal());

		const value_t inclination = std::acos(std::sqrt(zeroOneDist(gen)));
		const value_t azimuth = azimuthDist(gen);
		const auto localDirection = rotation * Geometry::_Sphere<value_t>::calcCartesianCoordinateUnitSphere(inclination, azimuth);
		const auto worldDirection = Transformations::localDirToWorldDir(*object, localDirection);
		Sample<value_t> newSample(ray_t(origin, worldDirection.getNormalized()));
		newSample.setBackwardResult(object, 0.0);
		return newSample;
	}

	/**
	 * Generate a random point in view space and a random point on the
	 * surface of a randomly chosen object. The point in view space is
	 * chosen as the origin and the direction is the vector to the point on
	 * the object's surface.
	 * 
	 * @see paragraph "Two-point distribution"
	 */
	Sample<value_t> generateTwoPointSample() {
		GeometryNode * object = getRandomObject();
		const auto triangle = getRandomTriangle(object);

		const auto u = zeroOneDist(gen);
		std::uniform_real_distribution<value_t> barycentricVDist(static_cast<value_t>(0.0), static_cast<value_t>(1.0) - u);
		const auto objectPoint = Transformations::localPosToWorldPos(*object, triangle.calcPoint(u, barycentricVDist(gen)));

		const auto viewSpacePoint = generateRandomViewSpacePoint();

		const auto direction = (objectPoint - viewSpacePoint).getNormalized();
		return Sample<value_t>(ray_t(viewSpacePoint, direction));
	}

	/**
	 * Generate a point on a plane by drawing from a two-dimensional gaussian
	 * distribution.
	 * 
	 * @param origin Point on the plane that will be used as the center of the
	 * gaussian distribution
	 * @param normal Normalized direction vector defining the plane
	 * @param standardDeviation Standard deviation of the gaussian distribution
	 * @return Random point
	 */
	vec3_t generateRandomPointOnPlane(const vec3_t & origin,
									  const vec3_t & normal,
									  value_t standardDeviation) const {
		const auto unitVecS = Geometry::Helper::createOrthogonal(normal);
		const auto unitVecT = normal.cross(unitVecS);

		std::normal_distribution<value_t> gaussianDist(0.0, standardDeviation);
		const auto s = gaussianDist(gen);
		const auto t = gaussianDist(gen);

		return origin + unitVecS * s + unitVecT * t;
	}

	/**
	 * Take a mutation candidate, mutate the origin and the termination point
	 * using two-dimensional gaussian distributions with the radii of the
	 * origin and of the termination object as standard deviation, and create
	 * a new sample out of the two points.
	 * 
	 * @see paragraph "Two-point mutation"
	 */
	Sample<value_t> generateTwoPointMutationSample() {
		const auto & cand = mutationCandidates.getMutationCandidate();
		const auto direction = (cand.termination - cand.origin).getNormalized();

		const auto radiusTermination = cand.terminationObject->getWorldBB().getBoundingSphereRadius();
		const auto mutatedTermination = generateRandomPointOnPlane(cand.termination, 
																   -direction, 
																   radiusTermination);

		const auto radiusOrigin = cand.originObject == nullptr ? radiusTermination : cand.originObject->getWorldBB().getBoundingSphereRadius();
		const auto mutatedOrigin = generateRandomPointOnPlane(cand.origin, 
															  direction, 
															  radiusOrigin);

		const auto rayOrigin = (mutatedOrigin + mutatedTermination) * 0.5;
		const auto rayDirection = (mutatedTermination - mutatedOrigin).getNormalized();
		return Sample<value_t>(ray_t(rayOrigin, rayDirection));
	}
	
	/**
	 * Take a mutation candidate, select one silhouette point, shoot discovery
	 * rays, and take the closest discovery ray that does not hit the object. 
	 * 
	 * @see paragraph "Silhouette mutation"
	 */
	Sample<value_t> generateSilhouetteMutationSample() {
		const auto & cand = mutationCandidates.getMutationCandidate();
		const auto direction = (cand.termination - cand.origin).getNormalized();

		const auto radius = cand.terminationObject->getWorldBB().getBoundingSphereRadius();
		const auto randomPlanePoint = generateRandomPointOnPlane(cand.termination,
																 -direction,
																 1000.0);
		const auto randomDirection = (randomPlanePoint - cand.termination).getNormalized();

		// Search on segment in randomDirection
		// In contrast to the article, use two times the radius here
		value_t searchBegin = static_cast<value_t>(0.0);
		value_t searchEnd = static_cast<value_t>(2.0 * radius);

		std::vector<ray_t> rays;
		rays.reserve(3);
		const auto segmentEnd = cand.termination + randomDirection * searchEnd;
		ray_t nearestNoHit(cand.origin,
						   (segmentEnd - cand.origin).getNormalized());

		// Depth 3
		for(uint_fast8_t depth = 0; depth < 3; ++depth) {
			// Quaternary search
			const value_t searchIncr = (searchEnd - searchBegin) / 4;
			value_t searchPos[3] = {searchBegin + searchIncr,
									searchBegin + 2 * searchIncr,
									searchBegin + 3 * searchIncr};
			for(const auto & s : searchPos) {
				const auto discoveryPos = cand.termination + randomDirection * s;
				rays.emplace_back(cand.origin,
								  (discoveryPos - cand.origin).getNormalized());
			}
			const auto results = RayCasting::RayCaster<value_t>::castRays(cand.terminationObject,
															  rays);
			// Check which ray did not hit the object and reduce the range
			if(results[0].first != cand.terminationObject) {
				searchEnd = searchPos[0];
				nearestNoHit = rays[0];
			} else if(results[1].first != cand.terminationObject) {
				searchBegin = searchPos[0];
				searchEnd = searchPos[1];
				nearestNoHit = rays[1];
			} else if(results[2].first != cand.terminationObject) {
				searchBegin = searchPos[1];
				searchEnd = searchPos[2];
				nearestNoHit = rays[2];
			} else {
				searchBegin = searchPos[2];
			}
			rays.clear();
		}
		return Sample<value_t>(nearestNoHit);
	}
	
	bool terminate() const {
		// The view space-direction distribution is the first entry
		const auto dist = sampleDists.front();
		const value_t epsilon = static_cast<value_t>(dist.numContributingSamples /* N_c */) / 
								static_cast<value_t>(dist.numSamples /* N */);
		const value_t k = static_cast<value_t>(0.5);
		const value_t P = static_cast<value_t>(0.9);
		// Equation (5)
		const auto numSamplesRequired = (1 - epsilon) / (k * k * epsilon * (1 - P));
		// One pixel per 1024 * 1024 pixels image
		const auto resolution = 1024.0 * 1024.0;
		const auto pixelError = (resolution * epsilon);
		std::cout << "pixelError=" << pixelError
					<< " epsilon=" << epsilon
					<< " numSamples=" << dist.numSamples
					<< " numSamplesRequired=" << numSamplesRequired << std::endl;
		return pixelError < 100.0 && dist.numSamples > numSamplesRequired;
	}
};

SampleDistributions::SampleDistributions(const Geometry::Box & viewSpaceBounds,
										 const GroupNode * scene) :
	impl(new Implementation<float>(viewSpaceBounds, scene)) {
}

SampleDistributions::~SampleDistributions() = default;

Sample<float> SampleDistributions::generateSample() const {
	return impl->generateSample();
}

void SampleDistributions::updateWithSample(const Sample<float> & sample,
										   const contribution_t & contribution,
										   ValuatedRegionNode * viewCell) {
	impl->updateWithSample(sample, contribution, viewCell);
}

void SampleDistributions::calculateDistributionProbabilities() {
	impl->updateDistributionProbabilities();
}

bool SampleDistributions::terminate() const {
	return impl->terminate();
}

}
}

#endif /* MINSG_EXT_ADAPTIVEGLOBALVISIBILITYSAMPLING */
