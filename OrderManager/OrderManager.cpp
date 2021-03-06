#include "OrderManager.h"

using namespace std;

void Order::ChangeOrderState(bool isPendingOrderUpdate)
{
	if (isPendingOrderUpdate || orderState != OrderState::NewPending && orderState != OrderState::ReplacePending && orderState != OrderState::Rejected)
	{
		// Changing Order State based on filled quantity and remaining quantity 
		if (filledQuantity == 0)
		{
			orderState = OrderState::Active;
		}
		else if (filledQuantity > 0 && remainingQuantity > 0)
		{
			orderState = OrderState::PartiallyFilled;
		}
		else //	remainingQuantity <= 0
		{
			orderState = OrderState::Completed;
		}
	}
}

void Order::replaceOrder(int newId, int deltaQuantity)
{
	id = newId;
	totalQuantity += deltaQuantity;
	remainingQuantity += deltaQuantity;
}


void OrderManager::updateNFQ(char side, int quantityFilled)
{
	if (side == 'B')
		nfq += quantityFilled;
	else
		nfq -= quantityFilled;
}

void OrderManager::updateCOV(char side, long double value)
{
	cov[side == 'B'] += value;
}

void OrderManager::updatePOV(char side, long double minValue, long double maxValue)
{
	pov_min[side == 'B'] += minValue;
	pov_max[side == 'B'] += maxValue;
}

void OrderManager::OnInsertOrderRequest(int id, char side, double price, int quantity)
{
	auto it = orders.find(id);
	if (it == orders.end())
	{
		orders.insert(make_pair(id, shared_ptr<Order>(new Order(id, side, price, quantity))));

		double pov = price * quantity;
		updatePOV(side, pov, pov);
	}
	else
	{
		// duplicate order id, log error
	}
}

void OrderManager::OnReplaceOrderRequest(int oldId, int newId, int deltaQuantity)
{
	auto it = orders.find(oldId);
	if (it != orders.end())
	{
		shared_ptr<Order> orderPtr = it->second;

		if (orderPtr->orderState != OrderState::NewPending && orderPtr->orderState != OrderState::ReplacePending)
		{
			replacePendingOrdersMap[oldId] = make_pair(newId, deltaQuantity);

			orderPtr->orderState = OrderState::ReplacePending;

			long minqty = orderPtr->remainingQuantity + ((deltaQuantity > 0) ? 0 : deltaQuantity);
			long maxqty = orderPtr->remainingQuantity + ((deltaQuantity < 0) ? 0 : deltaQuantity);

			double minPOV = orderPtr->Price() * minqty;
			double maxPOV = orderPtr->Price() * maxqty;

			updatePOV(orderPtr->Side(), minPOV, maxPOV);

			updateCOV(orderPtr->Side(), -(orderPtr->remainingQuantity * orderPtr->Price()));
		}
		else
		{
			// already pending request (NewPending or ReplacePending), log error
		}
	}
	else
	{
		// Order not present, log error
	}
}

void OrderManager::OnRequestAcknowledged(int id)
{
	auto it = orders.find(id);
	if (it != orders.end())
	{
		shared_ptr<Order> orderPtr = it->second;

		if (orderPtr->orderState == OrderState::NewPending)
		{
			orderPtr->ChangeOrderState(true);	// ChangeOrderState(isPendingUpdate = true)

			double pov = orderPtr->Price() * orderPtr->remainingQuantity;
			updatePOV(orderPtr->Side(), -pov, -pov);

			updateCOV(orderPtr->Side(), orderPtr->Price() * orderPtr->remainingQuantity);
		}
		else if (orderPtr->orderState == OrderState::ReplacePending) 
		{
			auto newId_deltaQty = replacePendingOrdersMap.find(id)->second;
			int newId = newId_deltaQty.first;	// newId
			int deltaQuantity = newId_deltaQty.second;	// deltaQuantity

			long minqty = orderPtr->remainingQuantity + ((deltaQuantity > 0) ? 0 : deltaQuantity);
			long maxqty = orderPtr->remainingQuantity + ((deltaQuantity < 0) ? 0 : deltaQuantity);

			double minPOV = orderPtr->Price() * minqty;
			double maxPOV = orderPtr->Price() * maxqty;

			updatePOV(orderPtr->Side(), -minPOV, -maxPOV);

			// this also updates remainingQuantity
			orderPtr->replaceOrder(newId, deltaQuantity);
			
			// the updated remainingQuatity is now applied to COV
			updateCOV(orderPtr->Side(), orderPtr->Price() * orderPtr->remainingQuantity);

			replacePendingOrdersMap.erase(id);
			orderPtr->ChangeOrderState(true);	// ChangeOrderState(isPendingUpdate = true));
		}
		else
		{
			// acknowledgement received for order in non-Pending State, log error
		}
	}
	else
	{
		// Order not present, log error
	}
}

void OrderManager::OnRequestRejected(int id)
{
	auto it = orders.find(id);
	if (it != orders.end())
	{
		shared_ptr<Order> orderPtr = it->second;

		if (orderPtr->orderState == OrderState::NewPending)
		{
			double pov = orderPtr->Price() * orderPtr->remainingQuantity;
			updatePOV(orderPtr->Side(), -pov, -pov);

			orderPtr->remainingQuantity = 0;
			orderPtr->orderState = OrderState::Rejected;
		}
		else if (orderPtr->orderState == OrderState::ReplacePending)
		{
			auto newId_deltaQty = replacePendingOrdersMap.find(id)->second;
			int deltaQuantity = newId_deltaQty.second;	// deltaQuantity

			long minqty = orderPtr->remainingQuantity + ((deltaQuantity > 0) ? 0: deltaQuantity);
			long maxqty = orderPtr->remainingQuantity + ((deltaQuantity < 0) ? 0 : deltaQuantity);

			double minPOV = orderPtr->Price() * minqty;
			double maxPOV = orderPtr->Price() * maxqty;

			updatePOV(orderPtr->Side(), -minPOV, -maxPOV);
			updateCOV(orderPtr->Side(), orderPtr->Price() * orderPtr->remainingQuantity);

			replacePendingOrdersMap.erase(id);
			orderPtr->ChangeOrderState(true);	// ChangeOrderState(isPendingUpdate = true)
		}
		else
		{
			// Rejection received for non pending order, log error
		}
	}
	else
	{
		// Order not present, log error
	}
}

void OrderManager::OnOrderFilled(int id, int quantityFilled)
{
	auto it = orders.find(id);
	if (it != orders.end())
	{
		shared_ptr<Order> orderPtr = it->second;

		if (orderPtr->orderState != OrderState::Rejected)
		{
			orderPtr->filledQuantity += quantityFilled;
			orderPtr->remainingQuantity -= quantityFilled;

			orderPtr->ChangeOrderState();

			updateNFQ(orderPtr->Side(), quantityFilled);

			if (orderPtr->orderState == OrderState::NewPending || orderPtr->orderState == OrderState::ReplacePending)
			{
				double pov = orderPtr->Price() * quantityFilled;
				updatePOV(orderPtr->Side(), -pov, -pov);
			}
			else // confirmed order (new or part filled)
			{
				updateCOV(orderPtr->Side(), -(orderPtr->Price() * quantityFilled));
			}
		}
		else
		{
			// fills received for rejected order, log error
		}
	}
}

int main()
{
	// Sample test case
	OrderManager manager;

	manager.OnInsertOrderRequest(100, 'B', 2, 10);
	manager.OnRequestAcknowledged(100);
	
	manager.OnInsertOrderRequest(105, 'B', 5, 20);
	manager.OnRequestAcknowledged(105);

	cout << manager.getNFQ() << " " << manager.getCOV('B') << " " << manager.getPOV_min('B') << " " << manager.getPOV_max('B') << endl;
	
	manager.OnReplaceOrderRequest(100, 101, 2);
	cout << manager.getNFQ() << " " << manager.getCOV('B') << " " << manager.getPOV_min('B') << " " << manager.getPOV_max('B') << endl;
	
	manager.OnOrderFilled(105, 5);
	cout << manager.getNFQ() << " " << manager.getCOV('B') << " " << manager.getPOV_min('B') << " " << manager.getPOV_max('B') << endl;

	manager.OnOrderFilled(100, 5);
	cout << manager.getNFQ() << " " << manager.getCOV('B') << " " << manager.getPOV_min('B') << " " << manager.getPOV_max('B') << endl;
	
	manager.OnRequestAcknowledged(100);
	cout << manager.getNFQ() << " " << manager.getCOV('B') << " " << manager.getPOV_min('B') << " " << manager.getPOV_max('B') << endl;

	manager.OnOrderFilled(100, 1);
	cout << manager.getNFQ() << " " << manager.getCOV('B') << " " << manager.getPOV_min('B') << " " << manager.getPOV_max('B') << endl;
	
	return 0;
}

