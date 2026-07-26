#pragma once

#include <stdexcept>

namespace order {
    
    enum class Side {
        BUY,
        SELL
    };

    enum class ExecutionLifeCycle {
        NONE,
        PARTIAL,
        FULL
    };

    class Order {
    private:
        unsigned int orderId;
        unsigned int orderArrival;

        unsigned int priceTick;
        unsigned int quantity;

        Side side;
        ExecutionLifeCycle execution;

    public:
        Order(unsigned int id, unsigned int tick, unsigned int quant, unsigned int arrival, Side s);

        unsigned int getOrderId() const;
        unsigned int getOrderArrival() const;

        unsigned int getPriceTick() const;
        unsigned int getQuantity() const;

        Side getSide() const;
        ExecutionLifeCycle getExecutionLevel() const;
        void setExecutionLevel(ExecutionLifeCycle newLevel);
    };

}