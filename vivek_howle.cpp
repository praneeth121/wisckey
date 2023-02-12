#include <bits/stdc++.h>
using namespace std;

int rq_id = 0;

class Vehicle {
public:
  bool is_available;
  int seats;
  int ride_start_time;
  int ride_end_time;

  bool is_vehicle_available(int curr_time) {
    if (is_available || ride_end_time < curr_time)
      return true;
    return false;
  }
};

class Car : public Vehicle {
public:
  Car() {
    is_available = 1;
    seats = 3;
    ride_end_time = -1;
    ride_start_time = -1;
  }
};

class Suv : public Vehicle {
public:
  Suv() {
    is_available = 1;
    seats = 4;
    ride_end_time = -1;
    ride_start_time = -1;
  }
};

class Van : public Vehicle {
public:
  Van() {
    is_available = 1;
    seats = 6;
    ride_end_time = -1;
    ride_start_time = -1;
  }
};

class Request {
public:
  bool is_valid;
  int no_of_persons;
  int ride_length;
  int rq_id;
  int time;
  int queue_idx;
};

// we are doing operator overloading through this
bool operator<(const Request &r1, const Request &r2) {

  // this will return true when second person
  // has greater height. Suppose we have p1.height=5
  // and p2.height=5.5 then the object which
  // have max height will be at the top(or
  // max priority)
  return r1.time > r2.time;
}

class Inventory {
public:
  vector<Car> cars;
  vector<Suv> suvs;
  vector<Van> vans;

  Inventory() {
    cars = vector<Car>(2);
    suvs = vector<Suv>(1);
    vans = vector<Van>(2);
  }

  Vehicle *get_vehicle(Request rq, int curr_time) {
    int seats = rq.no_of_persons;
    if (seats <= 3) {
      for (int i = 0; i < cars.size(); i++) {
        if (cars[i].is_vehicle_available(curr_time)) {
          cars[i].is_available = 1;
          return &cars[i];
        }
      }
    }
    if (seats <= 4) {
      for (int i = 0; i < suvs.size(); i++) {
        if (suvs[i].is_vehicle_available(curr_time)) {
          suvs[i].is_available = 1;
          return &suvs[i];
        }
      }
    }
    if (seats <= 6) {
      for (int i = 0; i < vans.size(); i++) {
        if (vans[i].is_vehicle_available(curr_time)) {
          vans[i].is_available = 1;
          return &vans[i];
        }
      }
    }
    Vehicle *dummy_vehicle = new Vehicle();
    dummy_vehicle->is_available = 0;
    return dummy_vehicle;
  }

  void print_inventory(int curr_time) {
    cout << "printing inventory" << endl;
    for (int i = 0; i < cars.size(); i++) {
      if (cars[i].is_vehicle_available(curr_time)) {
        cars[i].is_available = 0;
        cout << "\t" << "car " << i << ": is available" << endl;
      }
    }
    for (int i = 0; i < suvs.size(); i++) {
      if (suvs[i].is_vehicle_available(curr_time)) {
        suvs[i].is_available = 0;
        cout << "\t" << "suv " << i << ": is available" << endl;
      }
    }
    for (int i = 0; i < vans.size(); i++) {
      if (vans[i].is_vehicle_available(curr_time)) {
        vans[i].is_available = 0;
        cout << "\t" << "van " << i << ": is available" << endl;
      }
    }
    cout << endl;
  }
};

Request get_request(int time) {
  Request rq;
  rq.is_valid = 0;
  if (time % 2 == 0) {
    rq.is_valid = 1;
    rq.no_of_persons = (rand() % 6) + 1;
    rq.ride_length = (rand() % 20) + 1;
    rq.rq_id = rq_id;
    rq.time = time;
    rq_id++;
  }
  return rq;
}

int get_queue_id(int num) {
  if (num <= 3)
    return 0;
  if (num <= 4)
    return 1;
  return 2;
}

void serve_requests(vector<queue<Request>> &requests, Inventory &inventory,
                    int curr_time) {
  priority_queue<Request> q;
  for (int i = 0; i < 3; i++) {
    if (!requests[i].empty())
      q.push(requests[i].front());
  }
  bool served = false;
  while (!q.empty()) {
    Request rq = q.top();
    Vehicle *vehicle = inventory.get_vehicle(rq, curr_time);
    if (vehicle->is_available) {
      vehicle->is_available = false;
      vehicle->ride_start_time = curr_time;
      vehicle->ride_end_time = curr_time + rq.ride_length;
      requests[rq.queue_idx].pop();
      q.pop();
      if (!requests[rq.queue_idx].empty()) {
        q.push(requests[rq.queue_idx].front());
      }
    } else {
      q.pop();
      delete vehicle;
    }
  }
  return;
}

void print_all_pending_requests(vector<queue<Request>> &requests) {
    cout << "printing pending requests" << endl;
    for(int i = 0; i < 3;i++) {
        int q_size = requests[i].size();
        while(q_size) {
            Request rq = requests[i].front();
            cout << "\t" <<  "request: persons " << rq.no_of_persons << " ride_length " << rq.ride_length << endl;
            requests[i].pop();
            requests[i].push(rq); 
            q_size--;
        }
    }
    cout << endl;
}

int main() {
  int curr_time = 0;
  vector<queue<Request>> requests(3);
  Inventory inventory;

  while (1) {
    cout << "curr timestamp:" << curr_time << endl;
    Request rq = get_request(curr_time);
    if (rq.is_valid) {
      cout << "got a request for persons: " << rq.no_of_persons << " " << rq.ride_length << endl;
      int idx = get_queue_id(rq.no_of_persons);
      rq.queue_idx = idx;
      requests[idx].push(rq);
    }
    inventory.print_inventory(curr_time);
    print_all_pending_requests(requests);
    serve_requests(requests, inventory, curr_time);
    inventory.print_inventory(curr_time);

    curr_time += 1;

    if (curr_time > 100)
      break;
  }
  return 0;
}
