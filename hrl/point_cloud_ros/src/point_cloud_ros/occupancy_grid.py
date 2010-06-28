
import numpy as np, math

class occupancy_grid_3d():

    ##
    # @param resolution - 3x1 matrix. size of each cell (in meters) along
    #                     the different directions.
    def __init__(self, center, size, resolution, data):
        self.grid_shape = size/resolution
        tlb = center + size/2
        brf = center + size/2

        self.size = size
        self.grid = np.reshape(data, self.grid_shape)
        self.grid_shape = np.matrix(self.grid.shape).T
        self.resolution = resolution
        self.center = center

    ##
    # @param array - if not None then this will be used instead of self.grid
    # @return 3xN matrix of 3d coord of the cells which have occupancy >= occupancy_threshold
    def grid_to_points(self, array=None, occupancy_threshold=1):
        if array == None:
            array = self.grid

        idxs = np.where(array>=occupancy_threshold)
        x_idx = idxs[2]
        y_idx = idxs[1]
        z_idx = idxs[0]
        
        x = x_idx * self.resolution[0,0] + self.center[0,0] - self.size[0,0]/2
        y = y_idx * self.resolution[1,0] + self.center[1,0] - self.size[1,0]/2
        z = z_idx * self.resolution[2,0] + self.center[2,0] - self.size[2,0]/2

        return np.matrix(np.row_stack([x,y,z]))

if __name__ == '__main__':
    print 'Hello World'

