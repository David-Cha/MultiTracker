// ---------------------------------------------------------
//
//  meshsmoother.cpp
//  Tyson Brochu 2011
//  Christopher Batty, Fang Da 2014
//
//  Functions related to the tangent-space mesh smoothing operation.
//
// ---------------------------------------------------------

#include <meshsmoother.h>

#include <Eigen/Core>
#include <Eigen/Eigenvalues>
#include <impactzonesolver.h>
#include <collisionpipeline.h>
#include <collisionqueries.h>

#include <mat.h>
#include <nondestructivetrimesh.h>
#include <surftrack.h>
#include "trianglequality.h"

// ========================================================
//  NULL-space smoothing functions
// ========================================================

// ---------------------------------------------------------
///
/// Compute the maximum timestep that will not invert any triangle normals, using a quadratic solve as in [Jiao 2007].
///
// ---------------------------------------------------------

namespace LosTopos {
    
double MeshSmoother::compute_max_timestep_quadratic_solve( const std::vector<Vec3st>& tris,
                                                          const std::vector<Vec3d>& positions,
                                                          const std::vector<Vec3d>& displacements,
                                                          bool verbose )
{
    double max_beta = 1.0;
    
    double min_area = BIG_DOUBLE;
    
    for ( size_t i = 0; i < tris.size(); ++i )
    {
        if ( tris[i][0] == tris[i][1] ) { continue; }
        
        const Vec3d& x1 = positions[tris[i][0]];
        const Vec3d& x2 = positions[tris[i][1]];
        const Vec3d& x3 = positions[tris[i][2]];
        
        const Vec3d& u1 = displacements[tris[i][0]];
        const Vec3d& u2 = displacements[tris[i][1]];
        const Vec3d& u3 = displacements[tris[i][2]];
        
        Vec3d new_x1 = x1 + u1;
        Vec3d new_x2 = x2 + u2;
        Vec3d new_x3 = x3 + u3;
        
        const Vec3d c0 = cross( (x2-x1), (x3-x1) );
        const Vec3d c1 = cross( (x2-x1), (u3-u1) ) - cross( (x3-x1), (u2-u1) );
        const Vec3d c2 = cross( (u2-u1), (u3-u1) );
        const double a = dot(c0, c2);
        const double b = dot(c0, c1);
        const double c = dot(c0, c0);
        
        double beta = 1.0;
        
        min_area = min( min_area, c );
        
        if ( c < 1e-14 )
        {
            if ( verbose ) { std::cout << "super small triangle " << i << " (" << tris[i] << ")" << std::endl; }
        }
        
        if ( std::fabs(a) == 0 )
        {
            
            if ( ( std::fabs(b) > 1e-14 ) && ( -c / b >= 0.0 ) )
            {
                beta = -c / b;
            }
            else
            {
                if ( verbose )
                {
                    if ( std::fabs(b) < 1e-14 )
                    {
                        std::cout << "triangle " << i << ": ";
                        std::cout <<  "b == " << b << std::endl;
                    }
                }
            }
        }
        else
        {
            double descriminant = b*b - 4.0*a*c;
            
            if ( descriminant < 0.0  )
            {
                // Hmm, what does this mean?
                if ( verbose )
                {
                    std::cout << "triangle " << i << ": discriminant == " << descriminant << std::endl;
                }
                
                beta = 1.0;
            }
            else
            {
                double q;
                if ( b > 0.0 )
                {
                    q = -0.5 * ( b + sqrt( descriminant ) );
                }
                else
                {
                    q = -0.5 * ( b - sqrt( descriminant ) );
                }
                
                double beta_1 = q / a;
                double beta_2 = c / q;
                
                if ( beta_1 < 0.0 )
                {
                    if ( beta_2 < 0.0 )
                    {
                        assert( dot( triangle_normal(x1, x2, x3), triangle_normal(new_x1, new_x2, new_x3) ) > 0.0 );
                    }
                    else
                    {
                        beta = beta_2;
                    }
                }
                else
                {
                    if ( beta_2 < 0.0 )
                    {
                        beta = beta_1;
                    }
                    else if ( beta_1 < beta_2 )
                    {
                        beta = beta_1;
                    }
                    else
                    {
                        beta = beta_2;
                    }
                }
                
            }
        }
        
        bool changed = false;
        if ( beta < max_beta )
        {
            max_beta = 0.99 * beta;
            changed = true;
            
            if ( verbose )
            {
                std::cout << "changing beta --- triangle: " << i << std::endl;
                std::cout << "new max beta: " << max_beta << std::endl;
                std::cout << "a = " << a << ", b = " << b << ", c = " << c << std::endl;
            }
            
            if ( max_beta < 1e-4 )
            {
                //assert(0);
            }
            
        }
        
        new_x1 = x1 + max_beta * u1;
        new_x2 = x2 + max_beta * u2;
        new_x3 = x3 + max_beta * u3;
        
        Vec3d old_normal = cross(x2-x1, x3-x1);
        Vec3d new_normal = cross(new_x2-new_x1, new_x3-new_x1);
        
        if ( dot( old_normal, new_normal ) < 0.0 )
        {
            std::cout << "triangle " << i << ": " << tris[i] << std::endl;
            std::cout << "old normal: " << old_normal << std::endl;
            std::cout << "new normal: " << new_normal << std::endl;
            std::cout << "dot product: " << dot( triangle_normal(x1, x2, x3), triangle_normal(new_x1, new_x2, new_x3) ) << std::endl;
            std::cout << (changed ? "changed" : "not changed") << std::endl;
            std::cout << "beta: " << beta << std::endl;
            std::cout << "max beta: " << max_beta << std::endl;
        }
    }
    
    return max_beta;
}


// --------------------------------------------------------
///
/// Find a new vertex location using Null-space smoothing
///
// --------------------------------------------------------


void MeshSmoother::null_space_smooth_vertex( size_t v,
                                            std::vector<double>& triangle_areas,
                                            std::vector<Vec3d>& triangle_normals,
                                            std::vector<Vec3d>& triangle_centroids,
                                            Vec3d& displacement ) const
{
    if(m_surf.m_mesh.vertex_is_deleted(v)) return;
    
    const NonDestructiveTriMesh& mesh = m_surf.m_mesh;
    
    if ( mesh.m_vertex_to_triangle_map[v].empty() )
    {
        displacement = Vec3d(0,0,0);
        return;
    }
    
    const std::vector<size_t>& edges = mesh.m_vertex_to_edge_map[v];
    for ( size_t j = 0; j < edges.size(); ++j )
    {
        if ( mesh.m_edge_to_triangle_map[ edges[j] ].size() == 1 ) //boundary edge //TODO Handle boundary edges more wisely. (Treat as ridge).
        {
            displacement = Vec3d(0,0,0);
            return;
        }
    }
    
    const std::vector<size_t>& incident_triangles = mesh.m_vertex_to_triangle_map[v];
    
    //check if any of the angles are bad enough to justify smoothing
    bool worth_smoothing = false;
    for (int t = 0; t < incident_triangles.size(); ++t) {
       size_t i = incident_triangles[t];
       Vec3st tri = m_surf.m_mesh.m_tris[i];
       Vec3d v0 = m_surf.get_position(tri[0]);
       Vec3d v1 = m_surf.get_position(tri[1]);
       Vec3d v2 = m_surf.get_position(tri[2]);

       //check for fairly poor angle *cosines* (instead of angles, which requires a slow acos call)
       Vec3d cos_angles;
       triangle_angle_cosines(v0, v1, v2, cos_angles[0], cos_angles[1], cos_angles[2]);

       //Vec3d angles;
       //triangle_angles(v0, v1, v2, angles[0], angles[1], angles[2]);
       
      //what's a good cutoff here? too tight, not enough smoothing. too loose, costly for no reason.
       const double min_cos = cos(160.0 * M_PI / 180.0);
       const double max_cos = cos(20.0 * M_PI / 180.0);

       bool any_bad_angles_cos = 
          cos_angles[0] < min_cos || cos_angles[0] > max_cos ||
          cos_angles[1] < min_cos || cos_angles[1] > max_cos ||
          cos_angles[2] < min_cos || cos_angles[2] > max_cos;
       
       if (any_bad_angles_cos) {
          worth_smoothing = true;
          break;
       }
       
    }
    //worth_smoothing = true; //just always smooth for now.

    //cut out early if all the angles are in a somewhat reasonable range
    if (!worth_smoothing) {
       displacement = Vec3d(0, 0, 0);
       return;
    }

    //if we're being aggressive, instead do naive Laplacian smoothing of the vertex and then return.
    if(m_surf.m_aggressive_mode) {
        displacement = get_smoothing_displacement_naive(v, incident_triangles, triangle_areas, triangle_normals, triangle_centroids);
    }
    else {

       //identify vertices that are folded to be near-coplanar. (i.e. fail to be identified by Jiao's quadric)
       bool regularize_folded_feature = false;
       for (size_t i = 0; i < mesh.m_vertex_to_edge_map[v].size(); ++i) {
          size_t edge_id = mesh.m_vertex_to_edge_map[v][i];
          double angle = m_surf.get_largest_dihedral(edge_id, triangle_normals);
          if (M_PI - angle < m_sharp_fold_regularization_threshold) { //dihedral angle 170 degrees or more, i.e. two planes intersect at 10 degrees or less. consider it a "fold"
             regularize_folded_feature = true;
          }
       }

       if (regularize_folded_feature) {
          //Regularize these very sharp features. These situations typically indicate merging or "fold-overs"
          //so we try to encourage nicer merging by smoothing them in such a way that the sharp angle
          //becomes less sharp.

          //Figure out which volumetric region is the sharp one.

          //Collect all the regions
          std::set<int> incident_regions;
          for (size_t i = 0; i < m_surf.m_mesh.m_vertex_to_triangle_map[v].size(); ++i)  {
             size_t tri = m_surf.m_mesh.m_vertex_to_triangle_map[v][i];
             Vec2i region_pair = m_surf.m_mesh.get_triangle_label(tri);
             incident_regions.insert(region_pair[0]);
             incident_regions.insert(region_pair[1]);
          }

          //Find the sharpest one
          int sharpest_region = -1;
          double sharp_angle = 0;

          //for each incident edge...
          for (size_t i = 0; i < m_surf.m_mesh.m_vertex_to_edge_map[v].size(); ++i) {
             size_t edge = m_surf.m_mesh.m_vertex_to_edge_map[v][i];

             //let's only consider 3-way junctions, since 4-ways are more complex and unstable anyway
             if (m_surf.m_mesh.m_edge_to_triangle_map[edge].size() > 3)
                continue;

             //for each region...
             for (std::set<int>::iterator it = incident_regions.begin(); it != incident_regions.end(); ++it) {
                int region = *it;
                Vec3d normal_pair[2];
                //find the relevant dihedral angle between the two triangle normals
                //loop through the triangles, figure out each triangle's normal
                //there should only be two triangles on this edge bordering the same region, given that we consider edges with 3 or fewer tris.
                int next_tri_ind = 0;
                for (size_t j = 0; j < m_surf.m_mesh.m_edge_to_triangle_map[edge].size(); ++j) {
                   size_t tri = m_surf.m_mesh.m_edge_to_triangle_map[edge][j];
                   Vec2i label = m_surf.m_mesh.get_triangle_label(tri);
                   if (label[0] != region && label[1] != region) continue;

                   Vec3d normal = m_surf.get_triangle_normal_by_region(tri, region);
                   normal_pair[next_tri_ind] = normal;
                   ++next_tri_ind;
                }

                //compute dihedral angle between them
                double dihedral_angle = acos(dot(normal_pair[0], normal_pair[1]));
                if (dihedral_angle > sharp_angle) {
                   sharpest_region = region;
                   sharp_angle = dihedral_angle;
                }
             }
          }

          if (sharpest_region != -1 && M_PI - sharp_angle < m_sharp_fold_regularization_threshold) {
             //choose only that specific region/surface to smooth in the non-manifold case.
             std::vector<size_t> tri_set;
             for (size_t i = 0; i < incident_triangles.size(); ++i) {
                Vec2i label = mesh.get_triangle_label(incident_triangles[i]);
                if (label[0] == sharpest_region || label[1] == sharpest_region)
                   tri_set.push_back(incident_triangles[i]);
             }
             assert(tri_set.size() > 0);

             //This is useful in this case because quadric-based null-space smoothing identifies the tangent plane of the highly folded-triangles, and smooths only in that plane.
             //The result is that the vertices inside the sharp fold are pulled outward, slightly opens up the angle and regularizes the merge curve.
             displacement = get_smoothing_displacement(v, tri_set, triangle_areas, triangle_normals, triangle_centroids);
          }
          else {
             displacement = get_smoothing_displacement_dihedral(v, incident_triangles, triangle_areas, triangle_normals, triangle_centroids);
          }

       }
       else {
          displacement = get_smoothing_displacement_dihedral(v, incident_triangles, triangle_areas, triangle_normals, triangle_centroids);
       }
    }
    
    //Not sure why I'm doing this, but just copying it from code that I think Fang wrote.
    Vec3c solid = m_surf.vertex_is_solid_3(v);
    if (solid[0]) displacement[0] = 0; //TODO: What does this mean? Why set the floats to 0,1,2 respectively?
    if (solid[1]) displacement[1] = 1;
    if (solid[2]) displacement[2] = 2;
    
    if (mag(displacement) > 2*m_surf.m_max_edge_length) {
       std::cout << "Huge displacement request\n";
       displacement = Vec3d(0, 0, 0);
       return;
    }
    //std::cout << std::endl;
    //std::cout << "Displacement size: " << mag(displacement) << std::endl;

    //tentatively set the new position, in preparation for collision checks
    Vec3d newpos = m_surf.get_position(v) + displacement;
    m_surf.set_newposition(v, newpos);

    if (m_surf.m_collision_safety) {
       //check if the proposed motion would introduce a collision, and if so cancel it
       //this is likely to be cheaper than a full Bridson-style collision-handling loop
       bool collision = smooth_vertex_pseudo_motion_introduces_collision(v);
       
       if (collision) {
          /*std::cout << "Smoothing collision occurred\n";
          std::cout << "Position: " << m_surf.get_position(v) << std::endl;
          std::cout << "Newpos: " << newpos << std::endl;
          std::cout << "Displacement: " << displacement << std::endl;*/
          //revert the position and return.
          m_surf.set_newposition(v, m_surf.get_position(v));
          displacement = Vec3d(0, 0, 0);
          return;
       }
    }

    //finally apply the motion to the vertex, immediately!
    m_surf.set_position(v, newpos);
    //m_surf.set_newposition(v, newpos);
    //update the cached areas/centroids/normals
    for (int i = 0; i < incident_triangles.size(); ++i) {
       size_t t = incident_triangles[i];
       const Vec3st& tri = m_surf.m_mesh.get_triangle(t);
       if (tri[0] == tri[1])
       {
          triangle_areas[t] = 0;
          triangle_normals[t] = Vec3d(0, 0, 0);
          triangle_centroids[t] = Vec3d(0, 0, 0);
       }
       else
       {
          triangle_areas[t] = m_surf.get_triangle_area(t);
          triangle_normals[t] = m_surf.get_triangle_normal(t);
          triangle_centroids[t] = (m_surf.get_position(tri[0]) + m_surf.get_position(tri[1]) + m_surf.get_position(tri[2])) / 3.0;
       }
    }

}



//The classic null-space approach
Vec3d MeshSmoother::get_smoothing_displacement( size_t v,
                                               const std::vector<size_t>& triangles,
                                               const std::vector<double>& triangle_areas,
                                               const std::vector<Vec3d>& triangle_normals,
                                               const std::vector<Vec3d>& triangle_centroids) const {
    
    std::vector< Vec3d > N;
    std::vector< double > W;
    
    for ( size_t i = 0; i < triangles.size(); ++i )
    {
        size_t triangle_index = triangles[i];
        N.push_back( triangle_normals[triangle_index] );
        W.push_back( triangle_areas[triangle_index] );
    }
    
    Eigen::Matrix3d A_Eigen;
    A_Eigen.setZero();
    
    // Ax = b from N^TWni = N^TWd
    for ( size_t i = 0; i < N.size(); ++i )
    {
        A_Eigen(0, 0) += N[i][0] * W[i] * N[i][0];
        A_Eigen(1, 0) += N[i][1] * W[i] * N[i][0];
        A_Eigen(2, 0) += N[i][2] * W[i] * N[i][0];

        A_Eigen(0, 1) += N[i][0] * W[i] * N[i][1];
        A_Eigen(1, 1) += N[i][1] * W[i] * N[i][1];
        A_Eigen(2, 1) += N[i][2] * W[i] * N[i][1];

        A_Eigen(0, 2) += N[i][0] * W[i] * N[i][2];
        A_Eigen(1, 2) += N[i][1] * W[i] * N[i][2];
        A_Eigen(2, 2) += N[i][2] * W[i] * N[i][2];
    }

    // get eigen decomposition
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es;
    es.compute(A_Eigen, Eigen::ComputeEigenvectors);
    auto eigenvalues_Eigen = es.eigenvalues();
    double max_eig = std::max(eigenvalues_Eigen[0], std::max(eigenvalues_Eigen[1], eigenvalues_Eigen[2]));
    auto eigenvectors_Eigen = es.eigenvectors();
    
    std::vector<Vec3d> T2;
    for (unsigned int i = 0; i < 3; ++i)
    {
       if (eigenvalues_Eigen[i] < G_EIGENVALUE_RANK_RATIO * max_eig) {
          T2.push_back(Vec3d(eigenvectors_Eigen.col(i)[0], eigenvectors_Eigen.col(i)[1], eigenvectors_Eigen.col(i)[2]));
       }
    }
    
    
    Mat33d null_space_projection(0,0,0,0,0,0,0,0,0);
    for ( unsigned int row = 0; row < 3; ++row )
    {
        for ( unsigned int col = 0; col < 3; ++col )
        {
            for ( size_t i = 0; i < T2.size(); ++i )
            {
                null_space_projection(row, col) += T2[i][row] * T2[i][col];
            }
        }
    }
    
    Vec3d t(0,0,0);      // displacement
    double sum_areas = 0;
    
    for ( size_t i = 0; i < triangles.size(); ++i )
    {
        double area = triangle_areas[triangles[i]];
        sum_areas += area;
        Vec3d c = triangle_centroids[triangles[i]] - m_surf.get_position(v);
        t += area * c;
    }
    
    t = null_space_projection * t;
    t /= sum_areas;
    
    return t;
}


//Using dihedral angle to decide feature (edges and corners)
Vec3d MeshSmoother::get_smoothing_displacement_dihedral( size_t v,
                                                        const std::vector<size_t>& triangles,
                                                        const std::vector<double>& triangle_areas,
                                                        const std::vector<Vec3d>& triangle_normals,
                                                        const std::vector<Vec3d>& triangle_centroids) const
{
    int feature_edge_count = m_surf.vertex_feature_edge_count(v, triangle_normals);
   
    //Corner, don't smooth it at all.
    if(feature_edge_count >= 3)
        return Vec3d(0,0,0);
    
    //Do an eigen-decomposition to find the medial quadric, a la Jiao
    std::vector< Vec3d > N;
    std::vector< double > W;
    N.resize(triangles.size());
    W.resize(triangles.size());
    
    for ( size_t i = 0; i < triangles.size(); ++i )
    {
        size_t triangle_index = triangles[i];
        N[i] = triangle_normals[triangle_index];
        W[i] = triangle_areas[triangle_index];
    }
    
    Mat33d A(0,0,0,0,0,0,0,0,0);
    Eigen::Matrix3d A_Eigen;
    A_Eigen.setZero();
    // Ax = b from N^TWni = N^TWd
    for ( size_t i = 0; i < N.size(); ++i )
    {
        A(0,0) += N[i][0] * W[i] * N[i][0];
        A(1,0) += N[i][1] * W[i] * N[i][0];
        A(2,0) += N[i][2] * W[i] * N[i][0];
        
        A(0,1) += N[i][0] * W[i] * N[i][1];
        A(1,1) += N[i][1] * W[i] * N[i][1];
        A(2,1) += N[i][2] * W[i] * N[i][1];
        
        A(0,2) += N[i][0] * W[i] * N[i][2];
        A(1,2) += N[i][1] * W[i] * N[i][2];
        A(2,2) += N[i][2] * W[i] * N[i][2];

        A_Eigen(0, 0) += N[i][0] * W[i] * N[i][0];
        A_Eigen(1, 0) += N[i][1] * W[i] * N[i][0];
        A_Eigen(2, 0) += N[i][2] * W[i] * N[i][0];

        A_Eigen(0, 1) += N[i][0] * W[i] * N[i][1];
        A_Eigen(1, 1) += N[i][1] * W[i] * N[i][1];
        A_Eigen(2, 1) += N[i][2] * W[i] * N[i][1];

        A_Eigen(0, 2) += N[i][0] * W[i] * N[i][2];
        A_Eigen(1, 2) += N[i][1] * W[i] * N[i][2];
        A_Eigen(2, 2) += N[i][2] * W[i] * N[i][2];
        
    }
    
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es;
    es.compute(A_Eigen, Eigen::ComputeEigenvectors);
    if (es.info() != Eigen::Success) {
       std::cout << "Eigen decomposition failed" << std::endl;
       std::cout << "number of incident_triangles: " << triangles.size() << std::endl;
       for (size_t i = 0; i < triangles.size(); ++i)
       {
          size_t triangle_index = triangles[i];
          std::cout << "triangle: " << m_surf.m_mesh.get_triangle(triangle_index) << std::endl;
          std::cout << "normal: " << triangle_normals[triangle_index] << std::endl;
          std::cout << "area: " << triangle_areas[triangle_index] << std::endl;
       }

       assert(0);
    }

    auto eigenvalues_Eigen = es.eigenvalues();
    double eig_max = std::max(eigenvalues_Eigen[0], std::max(eigenvalues_Eigen[1], eigenvalues_Eigen[2]));
    double eig_min = std::min(eigenvalues_Eigen[0], std::min(eigenvalues_Eigen[1], eigenvalues_Eigen[2]));
    double eig_mid = std::max(min(eigenvalues_Eigen[0], eigenvalues_Eigen[1]), min(max(eigenvalues_Eigen[0], eigenvalues_Eigen[1]), eigenvalues_Eigen[2]));
    
    auto eigenvectors_Eigen = es.eigenvectors();
    auto eig_min_vec = eig_min == eigenvalues_Eigen[0] ? eigenvectors_Eigen.col(0) : (eig_min == eigenvalues_Eigen[1] ? eigenvectors_Eigen.col(1) : eigenvectors_Eigen.col(2));

    if(feature_edge_count == 0) {
       
        //do ordinary tangential smoothing (Laplacian smoothing, project out vertical (normal) component
        
        //Determine the normal per Jiao's approach
        //->using the medial quadric (see Identification of C1 and C2 Discontinuities for Surface Meshes in CAD, equation 2)
        
        //TODO Try other, less expensive normals to see if we can bring down the cost without sacrificing quality.
        //     Alternately, is there an equivalent normal to Jiao's that doesn't require an eigendecomposition?
        
        //TODO Apply smoothing selectively only if the geometry is worse than some threshold? Likewise
        //     can we adjust the collision detection/response used here to be localized?
        
        Vec3d Jiao_b(0,0,0);
        for ( size_t i = 0; i < N.size(); ++i )
        {
            Jiao_b += N[i]*W[i];
        }
        
       
        Vec3d Jiao_d(0, 0, 0);
        for (unsigned int i = 0; i < 3; ++i)
        {
           if (eigenvalues_Eigen[i] > G_EIGENVALUE_RANK_RATIO * eig_max)
           {
              auto eigenvector_Eigen = eigenvectors_Eigen.col(i);
              Vec3d eigenvector(eigenvector_Eigen[0], eigenvector_Eigen[1], eigenvector_Eigen[2]);
              Jiao_d += dot(Jiao_b, eigenvector)*eigenvector / eigenvalues_Eigen[i];
           }
        }
        
        Vec3d normal = Jiao_d;
        normalize(normal); //for good measure.
        
        Vec3d t(0,0,0);      // displacement
        double sum_areas = 0;
        Vec3d main_vert = m_surf.get_position(v);
        for ( size_t i = 0; i < triangles.size(); ++i )
        {
            size_t triangle_index = triangles[i];
            double area = triangle_areas[triangle_index];
            sum_areas += area;
            Vec3d c = triangle_centroids[triangle_index] - main_vert;
            t += area * c;
        }
        
        t /= sum_areas;
        
        //remove normal component of displacement. t = (I-nn^T)t
        t = t - normal*dot(normal, t);
        
        return t;
    }
    else  { //feature edge/ridge -> smooth along the ridge
       
        // compute basis for null space
        std::vector<Vec3d> T;
        
        
        //use only the eigenvector associated with the smallest eigenvalue, since we're assuming we are on a ridge, so there is one degree of freedom
        
        //Check that the ridge direction is well-conditioned before using it, per Jiao
        //"Identification of C1 and C2 Discontinuities for Surface Meshes in CAD"
        //Eigenvalues are sorted in ascending order. (Jiao numbers them in the opposite order from us.)
        const double epsilon_thresh = sqr(tan(5*M_PI/180));
        if (eig_min / eig_mid <= 0.7 && eig_mid / eig_max >= 0.00765) {
           T.push_back(Vec3d(eig_min_vec[0], eig_min_vec[1], eig_min_vec[2]));
        }
        else {
            //Try to concoct a reasonable alternative ridge/edge vector, when the quadric-based vector is ill-conditioned (e.g. surface seems flat, or actually sharply folded)
            if(feature_edge_count == 1) {
                //One feature edge; use its vector as the edge direction.
                for(size_t i = 0; i < m_surf.m_mesh.m_vertex_to_edge_map[v].size(); ++i) {
                    size_t edge = m_surf.m_mesh.m_vertex_to_edge_map[v][i];
                    if(m_surf.edge_is_feature(edge, triangle_normals)) {
                        Vec3d edgeVec = m_surf.get_position(m_surf.m_mesh.m_edges[edge][0]) - m_surf.get_position(m_surf.m_mesh.m_edges[edge][1]);
                        normalize(edgeVec);
                        T.push_back(edgeVec);
                    }
                }
            }
            else if(feature_edge_count == 2) {
                //Two feature edges. Use the vector between their midpoints.
                std::vector<Vec3d> edge_midpoints;
                for(size_t i = 0; i < m_surf.m_mesh.m_vertex_to_edge_map[v].size(); ++i) {
                    size_t edge = m_surf.m_mesh.m_vertex_to_edge_map[v][i];
                    if(m_surf.edge_is_feature(edge, triangle_normals)) {
                        Vec3d midpoint = 0.5*(m_surf.get_position(m_surf.m_mesh.m_edges[edge][0]) + m_surf.get_position(m_surf.m_mesh.m_edges[edge][1]));
                        edge_midpoints.push_back(midpoint);
                    }
                }
                Vec3d result = normalized(edge_midpoints[0] - edge_midpoints[1]);
                T.push_back(result);
            }
            else {
                std::cout <<"Error: Should not get here.\n";
                assert(false);
            }
            
        }
        
        Mat33d null_space_projection(0,0,0,0,0,0,0,0,0);
        for ( unsigned int row = 0; row < 3; ++row )
        {
            for ( unsigned int col = 0; col < 3; ++col )
            {
                for ( size_t i = 0; i < T.size(); ++i )
                {
                    null_space_projection(row, col) += T[i][row] * T[i][col];
                }
            }
        }
        
        Vec3d t(0,0,0);      // displacement
        double sum_areas = 0;
        
        for ( size_t i = 0; i < triangles.size(); ++i )
        {
            double area = triangle_areas[triangles[i]];
            sum_areas += area;
            Vec3d c = triangle_centroids[triangles[i]] - m_surf.get_position(v);
            t += area * c;
        }
        
        t = null_space_projection * t;
        t /= sum_areas;
        
        return t;
        
    }
    
    
}


//Basic laplacian smoothing (area-weighted)
Vec3d MeshSmoother::get_smoothing_displacement_naive( size_t v,
                                                     const std::vector<size_t>& triangles,
                                                     const std::vector<double>& triangle_areas,
                                                     const std::vector<Vec3d>& triangle_normals,
                                                     const std::vector<Vec3d>& triangle_centroids) const
{
    
    std::vector< Vec3d > N;
    std::vector< double > W;
    
    for ( size_t i = 0; i < triangles.size(); ++i )
    {
        size_t triangle_index = triangles[i];
        N.push_back( triangle_normals[triangle_index] );
        W.push_back( triangle_areas[triangle_index] );
    }
    
    Vec3d t(0,0,0);      // displacement
    double sum_areas = 0;
    
    for ( size_t i = 0; i < triangles.size(); ++i )
    {
        double area = triangle_areas[triangles[i]];
        sum_areas += area;
        Vec3d c = triangle_centroids[triangles[i]] - m_surf.get_position(v);
        t += area * c;
    }
    
    t /= sum_areas;
    
    return t;
}


// --------------------------------------------------------
///
/// Check if the "pseudo motion" for the smoothed vertex causes a collision
///
// --------------------------------------------------------

bool MeshSmoother::smooth_vertex_pseudo_motion_introduces_collision(size_t vertex) const
{
   assert(m_surf.m_collision_safety);

   // Get the set of triangles which move because of this motion
   std::vector<size_t>& moving_triangles = m_surf.m_mesh.m_vertex_to_triangle_map[vertex];

   // And the set of edges
   std::vector<size_t>& moving_edges = m_surf.m_mesh.m_vertex_to_edge_map[vertex];
   
   // Check for collisions, holding everything static except for the smoothed vertex

   CollisionCandidateSet collision_candidates;

   // triangle-point candidates
   for (size_t i = 0; i < moving_triangles.size(); ++i)
   {
      m_surf.m_collision_pipeline->add_triangle_candidates(moving_triangles[i], true, true, collision_candidates);
   }

   // point-triangle candidates
   m_surf.m_collision_pipeline->add_point_candidates(vertex, true, true, collision_candidates);

   // edge-edge candidates
   for (size_t i = 0; i < moving_edges.size(); ++i)
   {
      m_surf.m_collision_pipeline->add_edge_candidates(moving_edges[i], true, true, collision_candidates);
   }
      
   Collision collision;
   if (m_surf.m_collision_pipeline->any_collision(collision_candidates, collision))
       return true;

   // Also check proximity: if any proximity check returns zero distance, this collapse cannot be allowed either.
   // Because the CCD above is geometrically exact, it sometimes returns different result than the proximity check below (proximity
   //  distance = 0, but CCD says no collision). If distance is 0, the subsequent proximity handling will produce NaNs, so it is
   //  problematic too.
   
   for (size_t i = 0; i < collision_candidates.size(); i++)
   {
       const Vec3st & candidate = collision_candidates[i];
       if (candidate[2] == 1)
       {
           // edge-edge
           Vec2st e0 = m_surf.m_mesh.m_edges[candidate[0]];
           Vec2st e1 = m_surf.m_mesh.m_edges[candidate[1]];
           
           if (e0[0] == e0[1]) continue;
           if (e1[0] == e1[1]) continue;
           
           if (e0[0] != e1[0] && e0[0] != e1[1] && e0[1] != e1[0] && e0[1] != e1[1])
           {
               double distance, s0, s2;
               Vec3d normal;
               check_edge_edge_proximity(m_surf.get_newposition(e0[0]), m_surf.get_newposition(e0[1]), m_surf.get_newposition(e1[0]), m_surf.get_newposition(e1[1]), distance, s0, s2, normal);
               if (distance < m_surf.m_improve_collision_epsilon)
                   return true;
           }
           
       } else
       {
           // point-triangle
           size_t t = candidate[0];
           const Vec3st & tri = m_surf.m_mesh.get_triangle(t);
           size_t v = candidate[1];
           
           if (tri[0] == tri[1]) continue;
           
           if (tri[0] != v && tri[1] != v && tri[2] != v)
           {
               double distance, s1, s2, s3;
               Vec3d normal;
               check_point_triangle_proximity(m_surf.get_newposition(v), m_surf.get_newposition(tri[0]), m_surf.get_newposition(tri[1]), m_surf.get_newposition(tri[2]), distance, s1, s2, s3, normal);
               if (distance < m_surf.m_improve_collision_epsilon)
                   return true;
           }
       }
   }

    return false;
}




// --------------------------------------------------------
///
/// NULL-space smoothing
///
// --------------------------------------------------------

bool MeshSmoother::null_space_smoothing_pass( double dt )
{
    if ( m_surf.m_verbose )
    {
        std::cout << "---------------------- Los Topos: vertex redistribution ----------------------" << std::endl;
    }
    
    std::vector<double> triangle_areas;
    triangle_areas.reserve( m_surf.m_mesh.num_triangles());
    std::vector<Vec3d> triangle_normals;
    triangle_normals.reserve( m_surf.m_mesh.num_triangles());
    std::vector<Vec3d> triangle_centroids;
    triangle_centroids.reserve( m_surf.m_mesh.num_triangles());
    
    for ( size_t i = 0; i < m_surf.m_mesh.num_triangles(); ++i )
    {
        const Vec3st& tri = m_surf.m_mesh.get_triangle(i);
        if ( tri[0] == tri[1] )
        {
            triangle_areas.push_back( 0 );
            triangle_normals.push_back( Vec3d(0,0,0) );
            triangle_centroids.push_back( Vec3d(0,0,0) );
        }
        else
        {
            triangle_areas.push_back( m_surf.get_triangle_area( i ) );
            triangle_normals.push_back( m_surf.get_triangle_normal( i ) );
            triangle_centroids.push_back( (m_surf.get_position(tri[0]) + m_surf.get_position(tri[1]) + m_surf.get_position(tri[2])) / 3 );
        }
    }
    
    std::vector<Vec3d> displacements;
    displacements.resize( m_surf.get_num_vertices(), Vec3d(0) );
    
    double max_displacement = 1e-30;
    if(!m_surf.m_aggressive_mode)  {
        
        //in standard mode, smooth all vertices with null space smoothing
        for ( int i = 0; i < (int)m_surf.get_num_vertices(); ++i )
        {
            if ( !m_surf.vertex_is_all_solid(i) )
            {
                null_space_smooth_vertex( i, triangle_areas, triangle_normals, triangle_centroids, displacements[i] );
                max_displacement = max( max_displacement, mag( displacements[i] ) );
            }
        }
    }
    else {
       
        //in aggressive mode, identify only the triangles with bad angles, and smooth all of their vertices (with naive Laplacian smoothing)
        std::vector<bool> smoothed_already(m_surf.get_num_vertices(), false);
        for(size_t i = 0; i < m_surf.m_mesh.num_triangles(); ++i) {
            
            Vec3st tri = m_surf.m_mesh.m_tris[i];
            Vec3d v0 = m_surf.get_position(tri[0]);
            Vec3d v1 = m_surf.get_position(tri[1]);
            Vec3d v2 = m_surf.get_position(tri[2]);
            
            ////check for bad angles
           
            //check for bad angle *cosines* (instead of angles, which requires a slow acos call)
            Vec3d cos_angles;
            triangle_angle_cosines(v0, v1, v2, cos_angles[0], cos_angles[1], cos_angles[2]);

            bool any_bad_angles_cos = cos_angles[0] < m_surf.m_min_angle_cosine || cos_angles[0] > m_surf.m_max_angle_cosine ||
                                      cos_angles[1] < m_surf.m_min_angle_cosine || cos_angles[1] > m_surf.m_max_angle_cosine ||
                                      cos_angles[2] < m_surf.m_min_angle_cosine || cos_angles[2] > m_surf.m_max_angle_cosine;
            
            if (any_bad_angles_cos)
            {
                //std::cout << "Bad triangle angles: " << angles << std::endl;
                for(int j = 0; j < 3; ++j) {
                    size_t v = tri[j];
                    if ( !m_surf.vertex_is_all_solid(v) && !smoothed_already[v])
                    {
                        null_space_smooth_vertex(v, triangle_areas, triangle_normals, triangle_centroids, displacements[v]);
                        max_displacement = max( max_displacement, mag( displacements[v] ) );
                        smoothed_already[v] = true;
                    }
                }
            }
        }
    }
    
    /*

    // compute maximum dt (CB: Don't recall precisely why we disabled this. Jiao's tri inversion check is too conservative, probably.)
    double max_beta = 1.0; //compute_max_timestep_quadratic_solve( m_surf.m_mesh.get_triangles(), m_surf.m_positions, displacements, m_surf.m_verbose );
    
    if ( m_surf.m_verbose ) { std::cout << "max beta: " << max_beta << std::endl; }
    
    m_surf.m_velocities.resize( m_surf.get_num_vertices() );
    
    for ( size_t i = 0; i < m_surf.get_num_vertices(); ++i )
    {
        Vec3d displacement = (max_beta) * displacements[i];
        Vec3c solid = m_surf.vertex_is_solid_3(i);
        if (solid[0]) displacement[0] = 0; //TODO: What does this mean? Why set the floats to 0,1,2 respectively?
        if (solid[1]) displacement[1] = 1;
        if (solid[2]) displacement[2] = 2;
        m_surf.set_newposition( i, m_surf.get_position(i) + displacement );
        m_surf.m_velocities[i] = (m_surf.get_newposition(i) - m_surf.get_position(i)) / dt;
    }
    
    // repositioned locations stored in m_newpositions, but needs to be collision safe
    if ( m_surf.m_collision_safety )
    {
        
        bool all_collisions_handled = m_surf.m_collision_pipeline->handle_collisions(dt);
        
        if ( !all_collisions_handled )
        {
            std::cout << "Processing collisions in smoothing.\n";
            ImpactZoneSolver solver( m_surf );
            bool result = solver.inelastic_impact_zones(dt);
            
            if ( !result )
            {
                std::cout << "IIZ failed, moving to RIZ.\n";
                result = solver.rigid_impact_zones(dt);
            }
            
            if ( !result ) 
            {
                // couldn't fix collisions!
                std::cerr << "WARNING: Aborting mesh null-space smoothing due to CCD problem" << std::endl;
                return true;
            }
        }
        
        
        // TODO: Replace this with a cut-back and re-integrate
        // Actually, a call to DynamicSurface::integrate(dt) would be even better
        
        std::vector<Intersection> intersections;
        m_surf.get_intersections( false, true, intersections );
        
        if ( intersections.size() != 0 )
        {
            // couldn't fix collisions!
            std::cerr << "WARNING: Aborting mesh null-space smoothing due to CCD problem" << std::endl;
            return true;         
        }
        
    }
    
    // used to test convergence
    double max_position_change = 0.0;
    
    // Set positions
    for(size_t i = 0; i < m_surf.get_num_vertices(); i++)
    {
        max_position_change = max( max_position_change, mag( m_surf.get_newposition(i) - m_surf.get_position(i) ) );
    } 
    
    m_surf.set_positions_to_newpositions();
    
    
    if ( m_surf.m_verbose ) { std::cout << "max_position_change: " << max_position_change << std::endl; }
    
    // We will test convergence by checking whether the largest change in
    // position has magnitude less than: CONVERGENCE_TOL_SCALAR * average_edge_length  
    const static double CONVERGENCE_TOL_SCALAR = 1.0;   
    bool converged = false;
    if ( max_position_change < CONVERGENCE_TOL_SCALAR * m_surf.get_average_edge_length() )
    {
        converged = true;
    }
    */

    if (m_surf.m_mesheventcallback)
        m_surf.m_mesheventcallback->smoothing(m_surf);
    
    return true;
    //return !converged;
}
    
}

