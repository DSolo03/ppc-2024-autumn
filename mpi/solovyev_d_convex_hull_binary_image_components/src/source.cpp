#include <boost/mpi.hpp>
#include <boost/serialization/serialization.hpp>
#include <boost/serialization/vector.hpp>
#include <iostream>
#include <limits>
#include <random>
#include <stack>
#include <thread>

#include "mpi/solovyev_d_convex_hull_binary_image_components/include/header.hpp"

namespace solovyev_d_convex_hull_binary_image_components_mpi {

double Point::relativeAngle(Point other) const { return std::atan2((other.y * -1) - (y * -1), other.x - x); }

Image::Image() {
  image = std::vector<Point>{};
  size = 0;
  sizeX = 0;
  sizeY = 0;
}

Image::Image(std::vector<int> data, int dimX, int dimY) {
  for (size_t i = 0; i < data.size(); i++) {
    int pointY = i / dimX;
    int pointX = i - pointY * dimX;
    Point point = {pointX, pointY, data[i]};
    image.push_back(point);
  }
  size = data.size();
  sizeX = dimX;
  sizeY = dimY;
}

Point Image::getPoint(int x, int y) {
  if (x >= 0 && x <= sizeX && y >= 0 && y <= sizeY) {
    return image[y * sizeX + x];
  }
  Point point = {x, y, 0};
  return point;
}

void Image::setPoint(int x, int y, int value) { image[y * sizeX + x].value = value; }

std::vector<int> Image::getComponents() { return components; }

int Image::newComponent() {
  int id = components.back() + 1;
  components.push_back(id);
  return id;
}

void Image::clearComponents() {
  components.clear();
  components.push_back(0);
}

void Image::removeComponent(int n) {
  auto index = find(components.begin(), components.end(), n);
  if (index != components.end()) {
    components.erase(index);
  }
}

void Image::fixComponents() {
  for (size_t i = 0; i < components.size(); i++) {
    components[i] = i;
  }
}

void ConvexHullBinaryImageComponentsMPI::coutImage() {
  for (int y = 0; y < image.sizeY; y++) {
    for (int x = 0; x < image.sizeX; x++) {
      std::cout << image.getPoint(x, y).value << " ";
    }
    std::cout << std::endl;
  }
}

static int crossProduct(Point a, Point b, Point c) {
  return (b.x - a.x) * (-1 * c.y - -1 * a.y) - (-1 * b.y - -1 * a.y) * (c.x - a.x);
}

std::vector<Point> solovyev_d_convex_hull_binary_image_components_mpi::ConvexHullBinaryImageComponentsMPI::convexHull(
    std::vector<Point> component) {
  std::sort(component.begin(), component.end(), [](const Point &L, const Point &R) { return L.y < R.y; });
  Point point = component.back();
  std::sort(component.begin(), component.end(), [&](const Point &L, const Point &R) {
    if (point.relativeAngle(L) == point.relativeAngle(R)) {
      double distanceL = (L.x - point.x) * (L.x - point.x) + (L.y - point.y) * (L.y - point.y);
      double distanceR = (R.x - point.x) * (R.x - point.x) + (R.y - point.y) * (R.y - point.y);
      return distanceL < distanceR;
    }
    return point.relativeAngle(L) < point.relativeAngle(R);
  });
  std::stack<Point> hull;
  hull.push(component[0]);
  if (component.size() != 1) {
    hull.push(component[1]);
  }
  for (size_t i = 2; i < component.size(); ++i) {
    while (hull.size() > 1) {
      Point top = hull.top();
      hull.pop();
      Point nextToTop = hull.top();
      if (crossProduct(nextToTop, top, component[i]) > 0) {
        hull.push(top);
        break;
      }
    }
    hull.push(component[i]);
  }

  std::vector<Point> result;
  while (!hull.empty()) {
    result.push_back(hull.top());
    hull.pop();
  }
  std::reverse(result.begin(), result.end());
  return result;
}

bool ConvexHullBinaryImageComponentsMPI::pre_processing() {
  internal_order_test();

  // Init data vector
  if (world.rank() == 0) {
    int *input_ = reinterpret_cast<int *>(taskData->inputs[0]);
    int dimX = *reinterpret_cast<int *>(taskData->inputs[1]);
    int dimY = *reinterpret_cast<int *>(taskData->inputs[2]);
    std::vector<int> data(input_, input_ + taskData->inputs_count[0]);
    image = Image(data, dimX, dimY);
  }
  return true;
}

bool ConvexHullBinaryImageComponentsMPI::validation() {
  internal_order_test();
  if (world.rank() == 0) {
    return ((*reinterpret_cast<int *>(taskData->inputs[1]) >= 0 && *reinterpret_cast<int *>(taskData->inputs[2]) >= 0));
  } else {
    return true;
  }
}

bool ConvexHullBinaryImageComponentsMPI::run() {
  internal_order_test();
  // First phase
  if (world.rank() == 0) {
    image.clearComponents();
    results.clear();
    equivalenceTable.clear();
    for (int y = 0; y < image.sizeY; y++) {
      for (int x = 0; x < image.sizeX; x++) {
        Point point = image.getPoint(x, y);
        Point diag = image.getPoint(x - 1, y - 1);
        Point left = image.getPoint(x - 1, y);
        Point up = image.getPoint(x, y - 1);
        if (point.value != 0) {
          if (diag.value != 0) {
            image.setPoint(x, y, diag.value);
          } else {
            if (left.value == 0 && up.value == 0) {
              image.setPoint(x, y, image.newComponent());
            } else if (left.value == 0 && up.value != 0) {
              image.setPoint(x, y, up.value);
            } else if (left.value != 0 && up.value == 0) {
              image.setPoint(x, y, left.value);
            } else {
              image.setPoint(x, y, up.value);
              if (up.value != left.value) {
                equivalenceTable.push_back(eqUnit{left.value, up.value});
              }
            }
          }
        }
      }
    }

    // Second phase
    std::sort(equivalenceTable.begin(), equivalenceTable.end(),
              [&](const eqUnit &L, const eqUnit &R) { return L.replaceable > R.replaceable; });
    for (int y = 0; y < image.sizeY; y++) {
      for (int x = 0; x < image.sizeX; x++) {
        for (size_t i = 0; i < equivalenceTable.size(); i++) {
          if (image.getPoint(x, y).value == equivalenceTable[i].replaceable) {
            image.setPoint(x, y, equivalenceTable[i].replacement);
            image.removeComponent(equivalenceTable[i].replaceable);
          }
        }
      }
    }

    // Third phase
    for (int y = 0; y < image.sizeY; y++) {
      for (int x = 0; x < image.sizeX; x++) {
        for (size_t i = 0; i < image.getComponents().size(); i++) {
          if (image.getPoint(x, y).value == image.getComponents()[i]) {
            image.setPoint(x, y, i);
          }
        }
      }
    }
    image.fixComponents();
  }
  std::vector<std::vector<Point>> components;
  std::vector<std::vector<Point>> localComponents;

  std::vector<int> sendCounts(world.size(), 0);
  std::vector<int> displacements(world.size(), 0);

  if (world.rank() == 0) {
    for (size_t j = 1; j < image.getComponents().size(); j++) {
      std::vector<Point> component;
      for (int y = 0; y < image.sizeY; y++) {
        for (int x = 0; x < image.sizeX; x++) {
          if (image.getPoint(x, y).value == (int)j) {
            component.push_back(image.getPoint(x, y));
          }
        }
      }
      components.push_back(component);
    }
    int elementsCount = (components.size() / world.size());
    int remainder = (components.size() % world.size());
    sendCounts = std::vector<int>(world.size(), elementsCount);
    sendCounts[0] = sendCounts[0] + remainder;
    for (size_t i = 1; i < sendCounts.size(); i++) {
      displacements[i] = displacements[i - 1] + sendCounts[i - 1];
    }
  }
  boost::mpi::broadcast(world, sendCounts, 0);
  boost::mpi::broadcast(world, displacements, 0);
  localComponents.resize(sendCounts[world.rank()]);
  boost::mpi::scatterv(world, components, sendCounts, displacements, localComponents.data(), sendCounts[world.rank()],
                       0);
  for (size_t i = 0; i < localComponents.size(); i++) {
    localComponents[i] = convexHull(localComponents[i]);
  }
  boost::mpi::gatherv(world, localComponents, components.data(), sendCounts, displacements, 0);
  if (world.rank() == 0) {
    for (size_t i = 0; i < components.size(); i++) {
      /*       std::cout << "Convex hull for " << i << "-th component:" << std::endl;
            for (int k = 0; k < components[i].size(); k++) {
              std::cout << k << "-th coords: " << components[i][k].x << "," << components[i][k].y << std::endl;
            } */
      results.push_back(linearizePoints(components[i]));
    }
  }
  return true;
}

bool ConvexHullBinaryImageComponentsMPI::post_processing() {
  internal_order_test();
  world.barrier();
  if (world.rank() == 0) {
    for (size_t i = 0; i < results.size(); i++) {
      for (size_t j = 0; j < results[i].size(); j++) {
        reinterpret_cast<int *>(taskData->outputs[i])[j] = results[i][j];
      }
    }
  }
  return true;
}
}  // namespace solovyev_d_convex_hull_binary_image_components_mpi