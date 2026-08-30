#ifndef PF08_AURORA_GLSL
#define PF08_AURORA_GLSL

vec3 pf08_aurora_saturate(const vec3 colour, const float saturation) {
  const float luminance = dot(colour, vec3(0.2126, 0.7152, 0.0722));
  return max(mix(vec3(luminance), colour, saturation), vec3(0.0));
}

// Emission in a separate shell above the scattering atmosphere. Extending atmosphere_geometry.y to
// 240 km merely to fit the aurora would invalidate every existing LUT and waste samples in near-vacuum.
vec3 pf08_aurora_radiance(const pf08_sky_block sky, const vec3 origin, const vec3 direction,
                          const float background_luminance) {
  const float intensity = max(sky.aurora_appearance.x, 0.0);
  if (intensity <= 1e-5) return vec3(0.0);

  const float ground_radius = sky.atmosphere_geometry.x;
  const float lower_radius = ground_radius + sky.aurora_geometry.x;
  const float upper_radius = ground_radius + sky.aurora_geometry.y;
  const float layer_start = pf08_sphere_hit(origin, direction, lower_radius);
  const float layer_end = pf08_sphere_hit(origin, direction, upper_radius);
  if (layer_start < 0.0 || layer_end <= layer_start) return vec3(0.0);

  const float tilt = sky.aurora_magnetic.x;
  const float azimuth = sky.aurora_magnetic.y;
  const vec3 magnetic_pole = normalize(vec3(sin(azimuth) * sin(tilt), cos(tilt),
                                             -cos(azimuth) * sin(tilt)));
  const vec3 magnetic_east = normalize(cross(vec3(0.0, 1.0, 0.0), magnetic_pole) +
                                        vec3(1e-5, 0.0, 0.0));
  const vec3 magnetic_north = normalize(cross(magnetic_pole, magnetic_east));
  const float oval_angle = sky.aurora_geometry.z;
  const float oval_width = max(sky.aurora_geometry.w, radians(0.1));
  const float bands = max(sky.aurora_magnetic.z, 1.0);
  const float phase_time = sky.wind_params.w * sky.aurora_magnetic.w;
  const float density_control = clamp(sky.aurora_appearance.z, 0.0, 1.0);
  const float step_length = (layer_end - layer_start) / 8.0;
  vec3 radiance = vec3(0.0);

  const float oval_cosine = cos(oval_angle);
  const float oval_cosine_width = max(sin(oval_angle) * oval_width, 1e-4);
  for (int i = 0; i < 8; ++i) {
    const vec3 point = origin + direction * (layer_start + (float(i) + 0.5) * step_length);
    const float radius = length(point);
    const vec3 radial = point / max(radius, 1e-5);
    // Around a narrow oval, d(cos theta) = -sin(theta) dtheta. This is the same angular distance to
    // first order without an acos in every ray sample; the squared Gaussian does not care about sign.
    const float oval_distance = (dot(radial, magnetic_pole) - oval_cosine) / oval_cosine_width;
    const float oval = exp(-oval_distance * oval_distance * 1.8);
    if (oval < 1e-4) continue;

    const float longitude = atan(dot(radial, magnetic_north), dot(radial, magnetic_east));
    // The longitudinal field is constant along a radial column: that is what makes curtains vertical
    // instead of a painted wavy band. Two incommensurate harmonics split the oval into stable folds.
    const float broad = 0.5 + 0.5 * sin(longitude * bands - phase_time +
                                       sin(longitude * 3.0 + 0.7) * 1.4);
    const float fine = 0.5 + 0.5 * sin(longitude * bands * 2.37 + phase_time * 0.61 + 2.1);
    const float field = broad * (0.68 + 0.32 * fine);
    const float threshold = mix(0.82, 0.18, density_control);
    const float curtain = smoothstep(threshold - 0.10, threshold + 0.08, field);

    const float height = clamp((radius - lower_radius) / max(upper_radius - lower_radius, 1e-5), 0.0, 1.0);
    const float vertical_sine = max(sin(3.14159265358979323846 * height), 0.0);
    const float vertical_envelope = vertical_sine * sqrt(vertical_sine);
    const float green_line = exp(-pow((height - 0.26) / 0.31, 2.0));
    const float violet_line = exp(-pow((height - 0.72) / 0.27, 2.0));
    vec3 emission = vec3(0.18, 1.0, 0.34) * green_line +
                    vec3(0.48, 0.16, 1.0) * violet_line * 0.42;
    emission = pf08_aurora_saturate(emission, max(sky.aurora_appearance.y, 0.0));
    radiance += emission * oval * curtain * vertical_envelope * step_length;
  }

  // Aurora exists in daylight but normally loses the contrast contest. The explicit art parameter can
  // relax only that perceptual gate for fantasy worlds without moving the shell or magnetic oval.
  const float natural_visibility = 1.0 / (1.0 + background_luminance / 0.035);
  const float daylight = mix(natural_visibility, 1.0, clamp(sky.aurora_appearance.w, 0.0, 1.0));
  return radiance * (0.0024 * intensity * daylight);
}

#endif
